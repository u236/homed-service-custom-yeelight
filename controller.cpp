#include "controller.h"
#include "logger.h"

Controller::Controller(const QString &configFile) : HOMEd(SERVICE_VERSION, configFile), m_names(false)
{
    QList <QString> names = getConfig()->childGroups();

    for (int i = 0; i < names.count(); i++)
    {
        const QString &name = names.at(i);
        QString address;
        bool debug;

        if (name == "log" || name == "mqtt")
            continue;

        address = getConfig()->value(QString("%1/address").arg(name)).toString();

        if (address.isEmpty())
            continue;

        debug = getConfig()->value(QString("%1/debug").arg(name), false).toBool();
        Device device(new DeviceObject(address, name, debug));

        connect(device.data(), &DeviceObject::availabilityUpdated, this, &Controller::availabilityUpdated);
        connect(device.data(), &DeviceObject::propertiesUpdated, this, &Controller::propertiesUpdated);
        connect(device.data(), &DeviceObject::capabilitiesUpdated, this, &Controller::capabilitiesUpdated);

        m_devices.append(device);
        device->init();
    }

    m_discovery = new QUdpSocket(this);
    connect(m_discovery, &QUdpSocket::readyRead, this, &Controller::discoveryReadyRead);

    if (getConfig()->value("discovery/enabled", false).toBool())
        discover();
}

void Controller::discover(void)
{
    QByteArray datagram = "M-SEARCH * HTTP/1.1\r\nHOST: " MULTICAST_ADDRESS ":1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n\r\n";

    if (m_discovery->state() != QAbstractSocket::BoundState)
        m_discovery->bind();

    logInfo << "Discovery request sent, waiting for devices to respond...";
    m_discovery->writeDatagram(datagram, QHostAddress(MULTICAST_ADDRESS), MULTICAST_PORT);
}

void Controller::publishDevice(DeviceObject *device)
{
    device->setPublished();
    mqttPublish(mqttTopic("command/custom"), QJsonObject {{"action", "updateDevice"}, {"data", QJsonObject {{"real", true}, {"active", true}, {"cloud", false}, {"discovery", false}, {"id", device->id()}, {"service", QCoreApplication::applicationName()}, {"exposes", device->exposes()}, {"options", device->options()}}}});
}

void Controller::quit(void)
{
    for (int i = 0; i < m_devices.count(); i++)
        mqttPublish(mqttTopic("device/custom/%1").arg(m_names ? m_devices.at(i)->name() : m_devices.at(i)->id()), {{"status", "offline"}}, true);

    HOMEd::quit();
}

void Controller::mqttConnected(void)
{
    mqttSubscribe(mqttTopic("service/custom"));
    mqttPublishService();
}

void Controller::mqttReceived(const QByteArray &message, const QMqttTopicName &topic)
{
    QString subTopic = topic.name().replace(0, mqttTopic().length(), QString());
    QJsonObject json = QJsonDocument::fromJson(message).object();

    if (subTopic == "service/custom")
    {
        if (json.value("status").toString() != "online")
        {
            for (int i = 0; i < m_devices.count(); i++)
                mqttUnsubscribe(mqttTopic("td/custom/%1").arg(m_names ? m_devices.at(i)->name() : m_devices.at(i)->id()));

            return;
        }

        mqttSubscribe(mqttTopic("status/custom"));
    }
    else if (subTopic == "status/custom")
    {
        QJsonArray devices = json.value("devices").toArray();

        m_names = json.value("names").toBool();

        for (int i = 0; i < m_devices.count(); i++)
        {
            const Device &device = m_devices.at(i);
            bool check = true;

            for (auto it = devices.begin(); it != devices.end(); it++)
            {
                QJsonObject item = it->toObject();
                QString name = item.value("name").toString();

                if (item.value("id").toString() != device->id())
                    continue;

                if (name.isEmpty())
                    name = device->id();

                if (m_names && name != device->name())
                {
                    mqttUnsubscribe(mqttTopic("td/custom/%1").arg(device->name()));
                    device->setName(name);
                }

                check = false;
                break;
            }

            mqttSubscribe(mqttTopic("td/custom/%1").arg(m_names ? device->name() : device->id()));

            if (!check)
                device->setPublished();

            if (device->ready() && !device->published())
                publishDevice(device.data());
        }
    }
    else if (subTopic.startsWith("td/custom/"))
    {
        QString string = subTopic.split('/').last();

        for (int i = 0; i < m_devices.count(); i++)
        {
            const Device &device = m_devices.at(i);

            if ((m_names ? device->name() : device->id()) != string)
                continue;

            for (auto it = json.begin(); it != json.end(); it++)
                device->action(it.key(), it.value().toVariant());

            break;
        }
    }
}

void Controller::availabilityUpdated(Availability availability)
{
    DeviceObject *device = reinterpret_cast <DeviceObject*> (sender());
    QString status = availability == Availability::Online ? "online" : "offline";
    mqttPublish(mqttTopic("device/custom/%1").arg(m_names ? device->name() : device->id()), {{"status", status}}, true);
    logInfo << device << "is" << status;
}

void Controller::propertiesUpdated(const QMap <QString, QVariant> &properties)
{
    DeviceObject *device = reinterpret_cast <DeviceObject*> (sender());
    mqttPublish(mqttTopic("fd/custom/%1").arg(m_names ? device->name() : device->id()), QJsonObject::fromVariantMap(properties));
}

void Controller::capabilitiesUpdated(void)
{
    DeviceObject *device = reinterpret_cast <DeviceObject*> (sender());

    if (!device->published())
        publishDevice(device);
}

void Controller::discoveryReadyRead(void)
{
    while (m_discovery->hasPendingDatagrams())
    {
        QByteArray datagram;
        QMap <QString, QString> headers;
        QList <QByteArray> lines;
        QString location, id;

        datagram.resize(static_cast <int> (m_discovery->pendingDatagramSize()));
        m_discovery->readDatagram(datagram.data(), datagram.size());
        lines = datagram.split('\n');

        for (int i = 0; i < lines.count(); i++)
        {
            QByteArray line = lines.at(i).trimmed();
            int split = line.indexOf(':');

            if (split < 0)
                continue;

            headers.insert(QString(line.left(split)).trimmed().toLower(), QString(line.mid(split + 1)).trimmed());
        }

        location = headers.value("location");
        id = headers.value("id");

        if (!location.startsWith("yeelight://") || id.isEmpty() || m_discovered.contains(id))
            continue;

        m_discovered.append(id);
        logInfo << "Discovered device" << id.toUtf8().constData() << "model" << headers.value("model").toUtf8().constData() << "at" << location.mid(11).toUtf8().constData() << "supports:" << headers.value("support").toUtf8().constData();
    }
}
