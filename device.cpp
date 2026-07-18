#include <QJsonDocument>
#include <QRegExp>
#include <QtMath>
#include "color.h"
#include "device.h"
#include "logger.h"

DeviceObject::DeviceObject(const QString &address, const QString &id, bool debug) : QObject(nullptr), m_id(id), m_name(id), m_debug(debug), m_probe(new QUdpSocket(this)), m_socket(new QTcpSocket(this)), m_resetTimer(new QTimer(this)), m_updateTimer(new QTimer(this)), m_address(QHostAddress(address)), m_port(CONTROL_PORT), m_connected(false), m_sequence(1), m_availability(Availability::Unknown), m_lastSeen(QDateTime::currentMSecsSinceEpoch()), m_ready(false), m_background(false), m_ceiling(false), m_published(false)
{
    connect(m_probe, &QUdpSocket::readyRead, this, &DeviceObject::readyRead);

    connect(m_socket, &QTcpSocket::errorOccurred, this, &DeviceObject::socketError);
    connect(m_socket, &QTcpSocket::connected, this, &DeviceObject::socketConnected);
    connect(m_socket, &QTcpSocket::disconnected, this, &DeviceObject::socketDisconnected);
    connect(m_socket, &QTcpSocket::readyRead, this, &DeviceObject::readyRead);

    connect(m_resetTimer, &QTimer::timeout, this, &DeviceObject::reset);
    connect(m_updateTimer, &QTimer::timeout, this, &DeviceObject::update);

    m_resetTimer->setSingleShot(true);
    m_updateTimer->start(1000);
}

DeviceObject::~DeviceObject(void)
{
    if (m_connected)
        m_socket->disconnectFromHost();
}

void DeviceObject::init(void)
{
    if (m_address.isNull())
    {
        logWarning << this << "has invalid address";
        return;
    }

    if (m_socket->state() != QAbstractSocket::UnconnectedState)
    {
        m_socket->abort();
        m_connected = false;
    }

    if (m_probe->state() != QAbstractSocket::BoundState)
        m_probe->bind();

    m_buffer.clear();
    m_pending.clear();
    sendSearch();
    m_resetTimer->start(RESET_TIMEOUT);
}

void DeviceObject::updateAvailability(Availability availability)
{
    if (m_availability == availability)
        return;

    m_availability = availability;
    emit availabilityUpdated(availability);
}

void DeviceObject::sendSearch(void)
{
    QByteArray datagram = "M-SEARCH * HTTP/1.1\r\nHOST: " MULTICAST_ADDRESS ":1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n\r\n";
    logDebug(m_debug) << this << "discovery request sent";
    m_probe->writeDatagram(datagram, QHostAddress(MULTICAST_ADDRESS), MULTICAST_PORT);
}

void DeviceObject::sendCommand(const QString &method, const QJsonArray &params)
{
    QByteArray data = QJsonDocument(QJsonObject {{"id", m_sequence}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact).append("\r\n");
    logDebug(m_debug) << this << "command sent:" << data.trimmed();
    m_socket->write(data);
    m_sequence++;
}

void DeviceObject::setProperty(const QString &method, const QVariant &value)
{
    sendCommand(method, QJsonArray {QJsonValue::fromVariant(value), EFFECT, TRANSITION});
}

void DeviceObject::getProperties(const QList <QString> &names)
{
    QJsonArray params;

    if (names.isEmpty())
        return;

    for (int i = 0; i < names.count(); i++)
        params.append(names.at(i));

    if (m_pending.count() > 16)
        m_pending.clear();

    m_pending.insert(m_sequence, names);
    sendCommand("get_prop", params);
}

void DeviceObject::parseSearch(const QByteArray &datagram)
{
    QMap <QString, QVariant> headers;
    QList <QByteArray> lines = datagram.split('\n');
    QString location, host;
    quint16 port;

    for (int i = 0; i < lines.count(); i++)
    {
        QByteArray line = lines.at(i).trimmed();
        int split = line.indexOf(':');

        if (split < 0)
            continue;

        headers.insert(QString(line.left(split)).trimmed().toLower(), QString(line.mid(split + 1)).trimmed());
    }

    location = headers.value("location").toString();

    if (!location.startsWith("yeelight://"))
        return;

    host = location.mid(11).split(':').value(0);
    port = static_cast <quint16> (location.mid(11).split(':').value(1).toInt());

    if (host != m_address.toString())
        return;

    if (port)
        m_port = port;

    m_lastSeen = QDateTime::currentMSecsSinceEpoch();
    updateAvailability(Availability::Online);

    if (!m_ready)
    {
        buildCapabilities(headers.value("model").toString(), headers.value("support").toString().split(' ', Qt::SkipEmptyParts));
        logInfo << this << "model" << headers.value("model").toString() << "with exposes" << m_exposes << "discovered";
        m_ready = true;
        emit capabilitiesUpdated();
    }

    parseProperties(headers);

    if (!m_connected && m_socket->state() == QAbstractSocket::UnconnectedState)
        m_socket->connectToHost(m_address, m_port);
}

void DeviceObject::parseMessage(const QByteArray &message)
{
    QJsonObject json = QJsonDocument::fromJson(message).object();

    logDebug(m_debug) << this << "message received:" << message;

    if (json.isEmpty())
        return;

    m_lastSeen = QDateTime::currentMSecsSinceEpoch();
    updateAvailability(Availability::Online);

    if (json.value("method").toString() == "props")
    {
        parseProperties(json.value("params").toObject().toVariantMap());
        return;
    }

    if (json.contains("id"))
    {
        qint32 id = static_cast <qint32> (json.value("id").toInt());

        if (json.contains("error"))
        {
            logWarning << this << "command error:" << QJsonDocument(json.value("error").toObject()).toJson(QJsonDocument::Compact);
            m_pending.remove(id);
            return;
        }

        if (json.contains("result") && m_pending.contains(id))
        {
            QList <QString> names = m_pending.take(id);
            QJsonArray result = json.value("result").toArray();
            QMap <QString, QVariant> data;

            for (int i = 0; i < names.count() && i < result.count(); i++)
            {
                QString value = result.at(i).toString();

                if (!value.isEmpty())
                    data.insert(names.at(i), value);
            }

            if (!data.isEmpty())
                parseProperties(data);
        }
    }
}

void DeviceObject::buildCapabilities(const QString &model, const QList <QString> &support)
{
    for (int i = 0; i < support.count(); i++)
    {
        if (!support.at(i).startsWith("bg_"))
            continue;

        m_background = true;
        break;
    }

    if (m_background)
    {
        addLight(QString(), "_1", support);
        addLight("bg_", "_2", support);
    }
    else
        addLight(QString(), QString(), support);

    if (model.startsWith("ceil"))
    {
        m_ceiling = true;
        m_exposes.append("nightMode");
        m_options.insert("nightMode", QJsonObject {{"type", "toggle"}, {"icon", "mdi:weather-night"}});
        m_poll.append("active_mode");
        m_poll.append("active_bright");
    }
}

void DeviceObject::addLight(const QString &prefix, const QString &suffix, const QList <QString> &support)
{
    QString mode = prefix.isEmpty() ? "color_mode" : "bg_lmode";
    QJsonArray options;

    m_poll.append(prefix.isEmpty() && m_background ? QString("main_power") : QString("%1power").arg(prefix));

    if (support.contains(QString("%1set_bright").arg(prefix)))
    {
        options.append("level");
        m_poll.append(QString("%1bright").arg(prefix));
    }

    if (support.contains(QString("%1set_rgb").arg(prefix)) || support.contains(QString("%1set_hsv").arg(prefix)))
    {
        options.append("color");
        m_poll.append(QString("%1rgb").arg(prefix));
        m_poll.append(QString("%1hue").arg(prefix));
        m_poll.append(QString("%1sat").arg(prefix));
        m_poll.append(mode);
    }

    if (support.contains(QString("%1set_ct_abx").arg(prefix)))
    {
        options.append("colorTemperature");
        m_poll.append(QString("%1ct").arg(prefix));
        m_options.insert(QString("colorTemperature%1").arg(suffix), QJsonObject {{"min", COLOR_TEMPERATURE_MIN}, {"max", COLOR_TEMPERATURE_MAX}});
    }

    if (options.contains("color") && options.contains("colorTemperature"))
        options.append("colorMode");

    m_exposes.append(QString("light%1").arg(suffix));
    m_options.insert(QString("light%1").arg(suffix), options);
}

void DeviceObject::parseProperties(const QMap <QString, QVariant> &data)
{
    QMap <QString, QVariant> properties = m_properties;

    if (m_background)
    {
        mapProperties(QString(), "_1", data, properties);
        mapProperties("bg_", "_2", data, properties);
    }
    else
        mapProperties(QString(), QString(), data, properties);

    if (m_ceiling)
    {
        if (data.contains("active_mode"))
            properties.insert("nightMode", data.value("active_mode").toInt() == 1);

        if (data.contains("active_bright"))
            properties.insert(QString("level%1").arg(m_background ? "_1" : QString()), qRound(data.value("active_bright").toInt() * 255.0 / 100));
    }

    if (properties == m_properties)
        return;

    m_properties = properties;
    emit propertiesUpdated(properties);
}

void DeviceObject::mapProperties(const QString &prefix, const QString &suffix, const QMap <QString, QVariant> &data, QMap <QString, QVariant> &properties)
{
    QString key = prefix.isEmpty() ? "color_mode" : "bg_lmode";
    QString power = prefix.isEmpty() && m_background ? "main_power" : QString("%1power").arg(prefix);
    int mode = data.contains(key) ? data.value(key).toInt() : -1;

    if (data.contains(power))
        properties.insert(QString("status%1").arg(suffix), data.value(power).toString() == "on" ? "on" : "off");

    if (data.contains(QString("%1bright").arg(prefix)))
        properties.insert(QString("level%1").arg(suffix), qRound(data.value(QString("%1bright").arg(prefix)).toInt() * 255.0 / 100));

    if (data.contains(QString("%1ct").arg(prefix)) && data.value(QString("%1ct").arg(prefix)).toInt() > 0)
        properties.insert(QString("colorTemperature%1").arg(suffix), qRound(1000000.0 / data.value(QString("%1ct").arg(prefix)).toInt()));

    if (mode != -1)
        properties.insert(QString("colorMode%1").arg(suffix), mode != 2);

    if (mode == 3)
    {
        if (data.contains(QString("%1hue").arg(prefix)) && data.contains(QString("%1sat").arg(prefix)))
        {
            Color color = Color::fromHS(data.value(QString("%1hue").arg(prefix)).toDouble() / 360, data.value(QString("%1sat").arg(prefix)).toDouble() / 100);
            properties.insert(QString("color%1").arg(suffix), QVariantList {qRound(color.r() * 255), qRound(color.g() * 255), qRound(color.b() * 255)});
        }
    }
    else if (data.contains(QString("%1rgb").arg(prefix)))
    {
        int rgb = data.value(QString("%1rgb").arg(prefix)).toInt();
        properties.insert(QString("color%1").arg(suffix), QVariantList {rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF});
    }
}

void DeviceObject::action(const QString &name, const QVariant &data)
{
    QRegExp regExp("_(\\d+)$");

    if (regExp.indexIn(name) >= 0)
    {
        controlLight(name.left(name.length() - regExp.cap(0).length()), regExp.cap(0), data);
        return;
    }

    controlLight(name, QString(), data);
}

void DeviceObject::controlLight(const QString &name, const QString &suffix, const QVariant &data)
{
    QString prefix = suffix == "_2" ? "bg_" : QString();

    if (!m_connected)
        return;

    if (name == "status")
    {
        QString status = data.toString();

        if (status == "toggle")
        {
            sendCommand(QString("%1toggle").arg(prefix));
            return;
        }

        if (status != "on" && status != "off")
            return;

        setProperty(QString("%1set_power").arg(prefix), status);
        return;
    }

    if (name == "nightMode")
    {
        sendCommand("set_power", QJsonArray {"on", EFFECT, TRANSITION, data.toBool() ? 5 : 1});
        return;
    }

    if (m_properties.value(QString("status%1").arg(suffix)).toString() != "on")
        setProperty(QString("%1set_power").arg(prefix), "on");

    if (name == "level")
    {
        int bright = qRound(data.toInt() * 100.0 / 255);
        setProperty(QString("%1set_bright").arg(prefix), bright < 1 ? 1 : bright > 100 ? 100 : bright);
    }
    else if (name == "color")
    {
        QVariantList list = data.toList();
        int rgb;

        if (list.count() < 3)
            return;

        rgb = list.at(0).toInt() << 16 | list.at(1).toInt() << 8 | list.at(2).toInt();
        setProperty(QString("%1set_rgb").arg(prefix), rgb ? rgb : 1);
    }
    else if (name == "colorTemperature")
    {
        int mired = data.toInt(), kelvin;

        if (mired < 1)
            return;

        kelvin = qRound(1000000.0 / mired);
        setProperty(QString("%1set_ct_abx").arg(prefix), kelvin < 1700 ? 1700 : kelvin > 6500 ? 6500 : kelvin);
    }
}

void DeviceObject::readyRead(void)
{
    int index;

    if (sender() == m_probe)
    {
        while (m_probe->hasPendingDatagrams())
        {
            QByteArray datagram;

            datagram.resize(static_cast <int> (m_probe->pendingDatagramSize()));
            m_probe->readDatagram(datagram.data(), datagram.size());
            parseSearch(datagram);
        }

        return;
    }

    m_buffer.append(m_socket->readAll());

    if (m_buffer.length() > BUFFER_LENGTH_LIMIT)
        m_buffer.clear();

    while ((index = m_buffer.indexOf("\r\n")) >= 0)
    {
        QByteArray message = m_buffer.left(index);
        m_buffer.remove(0, index + 2);
        parseMessage(message);
    }
}

void DeviceObject::socketError(QAbstractSocket::SocketError error)
{
    Q_UNUSED(error)

    if (m_socket->state() == QAbstractSocket::ConnectedState)
        return;

    logWarning << this << "connection error:" << m_socket->errorString();
    updateAvailability(Availability::Offline);
    m_connected = false;
    m_resetTimer->start(RESET_TIMEOUT);
}

void DeviceObject::socketConnected(void)
{
    logInfo << this << "successfully connected to" << QString("%1:%2").arg(m_address.toString()).arg(m_port);
    m_socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    m_connected = true;
    m_buffer.clear();
    m_resetTimer->stop();
    getProperties(m_poll);
}

void DeviceObject::socketDisconnected(void)
{
    updateAvailability(Availability::Offline);
    m_connected = false;
    m_resetTimer->start(RESET_TIMEOUT);
}

void DeviceObject::reset(void)
{
    init();
}

void DeviceObject::ping(void)
{
    logDebug(m_debug) << this << "ping";
    getProperties(m_poll);
}

void DeviceObject::update(void)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (now > m_lastSeen + UNAVAILABLE_TIMEOUT)
        updateAvailability(Availability::Offline);

    if (m_connected && now > m_lastSeen + PING_TIMEOUT)
        ping();
}
