#include <math.h>
#include <netinet/tcp.h>
#include <QJsonDocument>
#include "color.h"
#include "device.h"
#include "logger.h"

DeviceObject::DeviceObject(const QString &address, const QString &id, bool debug) : QObject(nullptr), m_id(id), m_name(id), m_debug(debug), m_resetTimer(new QTimer(this)), m_updateTimer(new QTimer(this)), m_tcp(new QTcpSocket(this)), m_udp(new QUdpSocket(this)), m_address(QHostAddress(address)), m_port(CONTROL_PORT), m_connected(false), m_sequence(1), m_pending(0), m_background(false), m_ceiling(false), m_ready(false), m_published(false), m_availability(Availability::Unknown), m_lastSeen(QDateTime::currentMSecsSinceEpoch())
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
    bool bg = name.endsWith("_1"), check = m_properties.value(bg ? "status_1" : "status").toString() != "on";

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

            break;
        }

        case 1: // level
        {
            quint8 value = static_cast <quint8> (round(data.toInt() * 100.0 / 255));

            if (!value)
            {
                sendCommand(bg, "set_power", "off");
                break;
            }

            if (check)
                sendCommand(bg, "set_power", "on");

            sendCommand(bg, "set_bright", value > 100 ? 100 : value);
            break;
        }

        case 2: // color
        {
            QList <QVariant> list = data.toList();

            if (list.count() < 3)
                break;

            if (check)
                sendCommand(bg, "set_power", "on");

            sendCommand(bg, "set_rgb", list.at(0).toInt() << 16 | list.at(1).toInt() << 8 | list.at(2).toInt());
            break;
        }

        case 3: // colorTemperature
        {
            quint32 value = static_cast <quint32> (data.toInt());

            if (!value)
                break;

            value = static_cast <quint32> (round(1000000.0 / value));

            if (check)
                sendCommand(bg, "set_power", "on");

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
    emit availabilityUpdated(availability);
}

void DeviceObject::getProperties(void)
{
    if (m_items.isEmpty())
        return;

    m_pending = m_sequence;
    sendCommand(false, "get_prop", QJsonArray::fromStringList(m_items));
}

void DeviceObject::sendCommand(bool bg, const QString &method, const QJsonArray &data)
{
    QByteArray request = QJsonDocument(QJsonObject {{"id", m_sequence++}, {"method", bg ? QString("bg_%1").arg(method) : method}, {"params", data}}).toJson(QJsonDocument::Compact);
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







// NOT REVIEWED
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

    if (!m_connected && m_tcp->state() == QAbstractSocket::UnconnectedState)
        m_tcp->connectToHost(m_address, m_port);
}

// NOT REVIEWED
void DeviceObject::parseMessage(const QByteArray &message)
{
    QJsonObject json = QJsonDocument::fromJson(message).object();

    logDebug(m_debug) << this << "message received:" << message.constData();

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
            return;
        }

        if (json.contains("result") && id == m_pending)
        {
            QJsonArray result = json.value("result").toArray();
            QMap <QString, QVariant> data;

            for (int i = 0; i < m_items.count() && i < result.count(); i++)
            {
                QString value = result.at(i).toString();

                if (!value.isEmpty())
                    data.insert(m_items.at(i), value);
            }

            if (!data.isEmpty())
                parseProperties(data);
        }
    }
}

// NOT REVIEWED
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
        addLight(false, support);
        addLight(true, support);
    }
    else
        addLight(false, support);

    if (model.startsWith("ceil"))
    {
        m_ceiling = true;
        m_exposes.append("nightMode");
        m_options.insert("nightMode", QJsonObject {{"type", "toggle"}, {"icon", "mdi:weather-night"}});
        m_items.append("active_mode");
        m_items.append("active_bright");
    }
}

// NOT REVIEWED
void DeviceObject::addLight(bool bg, const QList <QString> &support)
{
    QJsonArray options;

    m_items.append(bg ? "bg_power" : m_background ? "main_power" : "power");

    if (support.contains(bg ? "bg_set_bright" : "set_bright"))
    {
        options.append("level");
        m_items.append(bg ? "bg_bright" : "bright");
    }

    if (support.contains(bg ? "bg_set_rgb" : "set_rgb") || support.contains(bg ? "bg_set_hsv" : "set_hsv"))
    {
        options.append("color");
        m_items.append(bg ? "bg_rgb" : "rgb");
        m_items.append(bg ? "bg_hue" : "hue");
        m_items.append(bg ? "bg_sat" : "sat");
        m_items.append(bg ? "bg_lmode" : "color_mode");
    }

    if (support.contains(bg ? "bg_set_ct_abx" : "set_ct_abx"))
    {
        options.append("colorTemperature");
        m_items.append(bg ? "bg_ct" : "ct");
        m_options.insert(bg ? "colorTemperature_1" : "colorTemperature", QJsonObject {{"min", COLOR_TEMPERATURE_MIN}, {"max", COLOR_TEMPERATURE_MAX}});
    }

    if (options.contains("color") && options.contains("colorTemperature"))
        options.append("colorMode");

    m_exposes.append(bg ? "light_1" : "light");
    m_options.insert(bg ? "light_1" : "light", options);
}

// NOT REVIEWED
void DeviceObject::parseProperties(const QMap <QString, QVariant> &data)
{
    QMap <QString, QVariant> properties = m_properties;

    if (m_background)
    {
        mapProperties(false, data, properties);
        mapProperties(true, data, properties);
    }
    else
        mapProperties(false, data, properties);

    if (m_ceiling)
    {
        if (data.contains("active_mode"))
            properties.insert("nightMode", data.value("active_mode").toInt() == 1);

        if (data.contains("active_bright"))
            properties.insert("level", qRound(data.value("active_bright").toInt() * 255.0 / 100));
    }

    if (properties.value("status").toString() == "off")
        properties.insert("level", 0);

    if (m_background && properties.value("status_1").toString() == "off")
        properties.insert("level_1", 0);

    if (properties == m_properties)
        return;

    m_properties = properties;
    emit propertiesUpdated(properties);
}

// NOT REVIEWED
void DeviceObject::mapProperties(bool bg, const QMap <QString, QVariant> &data, QMap <QString, QVariant> &properties)
{
    QString power = bg ? "bg_power" : m_background ? "main_power" : "power";
    QString key = bg ? "bg_lmode" : "color_mode";
    int mode = data.contains(key) ? data.value(key).toInt() : -1;

    if (data.contains(power))
    {
        if (m_connected && data.value(power).toString() == "on" && properties.value(bg ? "status_1" : "status").toString() != "on")
            getProperties();

        properties.insert(bg ? "status_1" : "status", data.value(power).toString() == "on" ? "on" : "off");
    }

    if (data.contains(bg ? "bg_bright" : "bright"))
        properties.insert(bg ? "level_1" : "level", qRound(data.value(bg ? "bg_bright" : "bright").toInt() * 255.0 / 100));

    if (data.contains(bg ? "bg_ct" : "ct") && data.value(bg ? "bg_ct" : "ct").toInt() > 0)
        properties.insert(bg ? "colorTemperature_1" : "colorTemperature", qRound(1000000.0 / data.value(bg ? "bg_ct" : "ct").toInt()));

    if (mode != -1)
        properties.insert(bg ? "colorMode_1" : "colorMode", mode != 2);

    if (mode == 3)
    {
        if (data.contains(bg ? "bg_hue" : "hue") && data.contains(bg ? "bg_sat" : "sat"))
        {
            Color color = Color::fromHS(data.value(bg ? "bg_hue" : "hue").toDouble() / 360, data.value(bg ? "bg_sat" : "sat").toDouble() / 100);
            properties.insert(bg ? "color_1" : "color", QVariantList {qRound(color.r() * 255), qRound(color.g() * 255), qRound(color.b() * 255)});
        }
    }
    else if (data.contains(bg ? "bg_rgb" : "rgb"))
    {
        int rgb = data.value(bg ? "bg_rgb" : "rgb").toInt();
        properties.insert(bg ? "color_1" : "color", QVariantList {rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF});
    }
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
        parseSearch(datagram);
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
