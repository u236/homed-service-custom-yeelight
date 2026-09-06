#include <math.h>
#include <netinet/tcp.h>
#include <QEventLoop>
#include <QJsonDocument>
#include "device.h"
#include "logger.h"

DeviceObject::DeviceObject(const QString &address, const QString &id, bool debug) : QObject(nullptr), m_id(id), m_name(id), m_debug(debug), m_resetTimer(new QTimer(this)), m_updateTimer(new QTimer(this)), m_tcp(new QTcpSocket(this)), m_udp(new QUdpSocket(this)), m_address(QHostAddress(address)), m_port(CONTROL_PORT), m_connected(false), m_sequence(1), m_pending(0), m_bg(false), m_ceiling(false), m_ready(false), m_published(false), m_off(false), m_availability(Availability::Unknown), m_lastSeen(QDateTime::currentMSecsSinceEpoch())
{
    connect(m_tcp, &QTcpSocket::errorOccurred, this, &DeviceObject::socketError);
    connect(m_tcp, &QTcpSocket::connected, this, &DeviceObject::socketConnected);
    connect(m_tcp, &QTcpSocket::disconnected, this, &DeviceObject::socketDisconnected);
    connect(m_tcp, &QTcpSocket::readyRead, this, &DeviceObject::readyRead);

    connect(m_udp, &QUdpSocket::readyRead, this, &DeviceObject::readyRead);

    connect(m_resetTimer, &QTimer::timeout, this, &DeviceObject::reset);
    connect(m_updateTimer, &QTimer::timeout, this, &DeviceObject::update);

    m_resetTimer->setSingleShot(true);
    m_updateTimer->start(1000);
}

DeviceObject::~DeviceObject(void)
{
    if (m_connected)
        m_tcp->disconnectFromHost();
}

void DeviceObject::init(void)
{
    QString request = QString("M-SEARCH * HTTP/1.1\r\nHOST: %1:1982\r\nMAN: \"ssdp:discover\"\r\nST: wifi_bulb\r\n\r\n").arg(MULTICAST_ADDRESS);

    if (m_address.isNull())
    {
        logWarning << this << "has invalid address";
        return;
    }

    if (m_tcp->state() != QAbstractSocket::UnconnectedState)
    {
        m_tcp->abort();
        m_connected = false;
    }

    if (m_udp->state() != QAbstractSocket::BoundState)
        m_udp->bind();

    m_buffer.clear();
    m_pending = 0;

    logDebug(m_debug) << this << "discovery request sent";
    m_udp->writeDatagram(request.toUtf8(), QHostAddress(MULTICAST_ADDRESS), MULTICAST_PORT);
    m_resetTimer->start(RESET_TIMEOUT);
}

void DeviceObject::action(const QString &name, const QVariant &data)
{
    QList <QString> actionList = {"status", "level", "color", "colorTemperature", "nightMode"};
    bool bg = name.endsWith("_1"), check = m_properties.value(suffix(bg, "status")).toString() != "on";

    if (!m_connected)
        return;

    switch (actionList.indexOf(name.split('_').value(0)))
    {
        case 0: // status
        {
            QList <QString> list = {"off", "on", "toggle"};
            QString value = data.toString();

            switch (list.indexOf(value))
            {
                case 0 ... 1: sendCommand(bg, "set_power", value);  break;
                case 2: sendCommand(bg, "toggle"); break;
            }

            if (!check && value != "on")
                m_off = true;

            break;
        }

        case 1: // level
        {
            quint8 value = static_cast <quint8> (round(data.toInt() * 100.0 / 255));

            if (!value)
            {
                sendCommand(bg, "set_power", "off");

                if (!check)
                    m_off = true;

                break;
            }

            if (check)
            {
                sendCommand(bg, "set_power", "on");
                waitForUpdate();
            }

            sendCommand(bg, "set_bright", value > 100 ? 100 : value);
            break;
        }

        case 2: // color
        {
            QList <QVariant> list = data.toList();

            if (list.count() < 3)
                break;

            if (check)
            {
                sendCommand(bg, "set_power", "on");
                waitForUpdate();
            }

            sendCommand(bg, "set_rgb", list.at(0).toInt() << 16 | list.at(1).toInt() << 8 | list.at(2).toInt());
            break;
        }

        case 3: // colorTemperature
        {
            quint32 value = data.toInt();

            if (!value)
                break;

            value = static_cast <quint32> (round(1000000.0 / value));

            if (check)
            {
                sendCommand(bg, "set_power", "on");
                waitForUpdate();
            }

            sendCommand(bg, "set_ct_abx", value < 1700 ? 1700 : value > 6500 ? 6500 : value);
            break;
        }

        case 4: // nightMode
        {
            sendCommand(false, "set_power", "on", data.toBool() ? 5 : 1);
            break;
        }
    }
}

void DeviceObject::updateAvailability(Availability availability)
{
    if (m_availability == availability)
        return;

    m_availability = availability;
    emit availabilityUpdated();
}

void DeviceObject::addLight(bool bg, const QList <QString> &support)
{
    QJsonArray options;

    m_items.append(bg ? "bg_power" : m_bg ? "main_power" : "power");

    if (support.contains(prefix(bg, "set_bright")))
    {
        options.append("level");
        m_items.append(prefix(bg, "bright"));
    }

    if (support.contains(prefix(bg, "set_rgb")))
    {
        options.append("color");
        m_items.append(prefix(bg, "rgb"));
        m_items.append(bg ? "bg_lmode" : "color_mode");
    }

    if (support.contains(prefix(bg, "set_ct_abx")))
    {
        options.append("colorTemperature");
        m_options.insert(suffix(bg, "colorTemperature"), QJsonObject {{"min", 153}, {"max", 370}});
        m_items.append(prefix(bg, "ct"));
    }

    if (options.contains("color") && options.contains("colorTemperature"))
        options.append("colorMode");

    m_exposes.append(suffix(bg, "light"));

    if (options.isEmpty())
        return;

    m_options.insert(suffix(bg, "light"), options);
}

void DeviceObject::sendCommand(bool bg, const QString &method, const QJsonArray &data)
{
    QByteArray request = QJsonDocument(QJsonObject {{"id", m_sequence++}, {"method", prefix(bg, method)}, {"params", data}}).toJson(QJsonDocument::Compact);
    logDebug(m_debug) << this << "command sent:" << request.constData();
    m_tcp->write(request.append("\r\n"));
}

void DeviceObject::sendCommand(bool bg, const QString &method, const QVariant &value, const QVariant &mode)
{
    QJsonArray data = {QJsonValue::fromVariant(value), "smooth", 500};

    if (mode.isValid())
        data.append(QJsonValue::fromVariant(mode));

    sendCommand(bg, method, data);
}

void DeviceObject::waitForUpdate(void)
{
    QEventLoop loop;
    QTimer timer;

    connect(this, &DeviceObject::propertiesUpdated, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.setSingleShot(true);
    timer.start(UPDATE_TIMEOUT);
    loop.exec();
}

void DeviceObject::getProperties(void)
{
    if (m_items.isEmpty())
        return;

    m_pending = m_sequence;
    sendCommand(false, "get_prop", QJsonArray::fromStringList(m_items));
}

void DeviceObject::parseProperties(bool bg, const QMap <QString, QVariant> &data, QMap <QString, QVariant> &properties)
{
    QString power = bg ? "bg_power" : m_bg ? "main_power" : "power", key = bg ? "bg_lmode" : "color_mode";
    int mode = data.contains(key) ? data.value(key).toInt() : -1;

    if (data.contains(power))
    {
        QString value = data.value(power).toString();

        if (m_connected && value == "on" && (m_off || properties.value(suffix(bg, "status")).toString() != "on"))
            getProperties();

        properties.insert(suffix(bg, "status"), value);
        m_off = false;
    }

    if (data.contains(prefix(bg, "bright")))
        properties.insert(suffix(bg, "level"), static_cast <quint8> (round(data.value(prefix(bg, "bright")).toInt() * 255 / 100.0)));

    if (data.contains(prefix(bg, "rgb")))
    {
        int value = data.value(prefix(bg, "rgb")).toInt();
        properties.insert(suffix(bg, "color"), QVariantList {value >> 16 & 0xFF, value >> 8 & 0xFF, value & 0xFF});
    }

    if (data.contains(prefix(bg, "ct")) && data.value(prefix(bg, "ct")).toInt() > 0)
        properties.insert(suffix(bg, "colorTemperature"), static_cast <quint16> (round(1000000.0 / data.value(prefix(bg, "ct")).toInt())));

    if (mode >= 0)
        properties.insert(suffix(bg, "colorMode"), mode != 2);
}

void DeviceObject::parseProperties(const QMap <QString, QVariant> &data)
{
    QMap <QString, QVariant> properties = m_properties;

    for (int i = 0; i < (m_bg ? 2 : 1); i++)
    {
        parseProperties(i, data, properties);

        if (properties.value(suffix(i, "status")).toString() == "on" || !m_options.value(suffix(i, "light")).toArray().contains("level"))
            continue;

        properties.insert(suffix(i, "level"), 0);
    }

    if (m_ceiling)
    {
        if (data.contains("active_bright") && properties.value("status").toString() == "on")
            properties.insert("level", static_cast <quint8> (round(data.value("active_bright").toInt() * 255 / 100.0)));

        if (data.contains("active_mode"))
            properties.insert("nightMode", data.value("active_mode").toInt() == 1);
    }

    if (m_properties == properties)
        return;

    m_properties = properties;
    emit propertiesUpdated();
}

void DeviceObject::parseMessage(const QByteArray &message)
{
    QJsonObject json = QJsonDocument::fromJson(message).object();

    logDebug(m_debug) << this << "message received:" << message.constData();
    m_lastSeen = QDateTime::currentMSecsSinceEpoch();
    updateAvailability(Availability::Online);

    if (json.value("method").toString() == "props")
    {
        parseProperties(json.value("params").toObject().toVariantMap());
        return;
    }

    if (!json.contains("id"))
        return;

    if (json.contains("error"))
    {
        logWarning << this << "command error:" << QJsonDocument(json.value("error").toObject()).toJson(QJsonDocument::Compact);
        return;
    }

    if (json.contains("result") && m_pending == json.value("id").toInt())
    {
        QJsonArray result = json.value("result").toArray();
        QMap <QString, QVariant> data;

        for (int i = 0; i < m_items.count() && i < result.count(); i++)
        {
            QString value = result.at(i).toString();

            if (value.isEmpty())
                continue;

            data.insert(m_items.at(i), value);
        }

        if (data.isEmpty())
            return;

        parseProperties(data);
    }
}

void DeviceObject::discovery(const QByteArray &datagram)
{
    QList <QString> list = QString(datagram).split("\r\n");
    QMap <QString, QVariant> headers;
    QString location, host;

    for (int i = 0; i < list.count(); i++)
    {
        int index = list.at(i).indexOf(':');

        if (index < 0)
            continue;

        headers.insert(list.at(i).mid(0, index).trimmed().toLower(), list.at(i).mid(index + 1).trimmed());
    }

    location = headers.value("location").toString();

    if (!location.startsWith("yeelight://"))
        return;

    list = location.mid(11).split(':');
    host = list.value(0);

    if (host != m_address.toString())
        return;

    m_port = static_cast <quint16> (list.value(1).toInt());
    m_lastSeen = QDateTime::currentMSecsSinceEpoch();
    updateAvailability(Availability::Online);

    if (!m_ready)
    {
        QList <QString> support = headers.value("support").toString().split(0x20, Qt::SkipEmptyParts);
        QString model = headers.value("model").toString();

        logDebug(m_debug) << this << "support:" << support.join(", ").toUtf8().constData();

        for (int i = 0; i < support.count(); i++)
        {
            if (!support.at(i).startsWith("bg_"))
                continue;

            m_bg = true;
            break;
        }

        for (int i = 0; i < (m_bg ? 2 : 1); i++)
            addLight(i, support);

        if (model.startsWith("ceil"))
        {
            m_exposes.append("nightMode");
            m_options.insert("nightMode", QJsonObject {{"type", "toggle"}, {"icon", "mdi:weather-night"}});
            m_items.append("active_mode");
            m_items.append("active_bright");
            m_ceiling = true;
        }

        logInfo << this << "model" << model << "with exposes" << m_exposes << "discovered"; // TODO: clean up
        m_ready = true;
        emit deviceUpdated();
    }

    parseProperties(headers);

    if (m_connected || m_tcp->state() != QAbstractSocket::UnconnectedState)
        return;

    m_tcp->connectToHost(m_address, m_port);
}

void DeviceObject::socketError(QAbstractSocket::SocketError error)
{
    logWarning << this << "connection error:" << error;
    updateAvailability(Availability::Offline);
    m_resetTimer->start(RESET_TIMEOUT);
    m_connected = false;
}

void DeviceObject::socketConnected(void)
{
    int descriptor = m_tcp->socketDescriptor(), keepAlive = 1, interval = 10, count = 3;

    setsockopt(descriptor, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(keepAlive));
    setsockopt(descriptor, SOL_TCP, TCP_KEEPIDLE, &interval, sizeof(interval));
    setsockopt(descriptor, SOL_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(descriptor, SOL_TCP, TCP_KEEPCNT, &count, sizeof(count));

    logInfo << this << "successfully connected to" << QString("%1:%2").arg(m_address.toString()).arg(m_port);
    m_resetTimer->stop();
    m_connected = true;
    m_buffer.clear();
    getProperties();
}

void DeviceObject::socketDisconnected(void)
{
    updateAvailability(Availability::Offline);
    m_resetTimer->start(RESET_TIMEOUT);
    m_connected = false;
}

void DeviceObject::readyRead(void)
{
    if (sender() == m_tcp)
    {
        m_buffer.append(m_tcp->readAll());

        if (m_buffer.length() > BUFFER_LENGTH_LIMIT)
            m_buffer.clear();

        while (m_buffer.contains("\r\n"))
        {
            QByteArray message = m_buffer.mid(0, m_buffer.indexOf("\r\n"));
            m_buffer.remove(0, message.length() + 2);
            parseMessage(message);
        }

        return;
    }

    while (m_udp->hasPendingDatagrams())
    {
        QByteArray datagram;
        datagram.resize(m_udp->pendingDatagramSize());
        m_udp->readDatagram(datagram.data(), datagram.size());
        discovery(datagram);
    }
}

void DeviceObject::reset(void)
{
    init();
}

void DeviceObject::update(void)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (now > m_lastSeen + UNAVAILABLE_TIMEOUT)
        updateAvailability(Availability::Offline);

    if (now > m_lastSeen + PING_TIMEOUT && m_connected)
    {
        logDebug(m_debug) << this << "ping";
        getProperties();
    }
}
