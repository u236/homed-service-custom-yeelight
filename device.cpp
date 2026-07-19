#include <netinet/tcp.h>
#include <QJsonDocument>
#include "color.h"
#include "device.h"
#include "logger.h"

DeviceObject::DeviceObject(const QString &address, const QString &id, bool debug) : QObject(nullptr), m_id(id), m_name(id), m_debug(debug), m_resetTimer(new QTimer(this)), m_updateTimer(new QTimer(this)), m_tcp(new QTcpSocket(this)), m_udp(new QUdpSocket(this)), m_address(QHostAddress(address)), m_port(CONTROL_PORT), m_connected(false), m_sequence(1), m_background(false), m_ceiling(false), m_ready(false), m_published(false), m_availability(Availability::Unknown), m_lastSeen(QDateTime::currentMSecsSinceEpoch())
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
    m_pending.clear();

    logDebug(m_debug) << this << "discovery request sent";
    m_udp->writeDatagram(request.toUtf8(), QHostAddress(MULTICAST_ADDRESS), MULTICAST_PORT);
    m_resetTimer->start(RESET_TIMEOUT);
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
    if (m_poll.isEmpty())
        return;

    if (m_pending.count() > 16)
        m_pending.clear();

    m_pending.insert(m_sequence, m_poll);
    sendCommand("get_prop", QJsonArray::fromStringList(m_poll));
}

void DeviceObject::sendCommand(const QString &method, const QJsonArray &data)
{
    QByteArray request = QJsonDocument(QJsonObject {{"id", m_sequence++}, {"method", method}, {"params", data}}).toJson(QJsonDocument::Compact);
    logDebug(m_debug) << this << "command sent:" << request.constData();
    m_tcp->write(request.append("\r\n"));
}

void DeviceObject::sendCommand(const QString &method, const QVariant &value, const QVariant &mode)
{
    QJsonArray data {QJsonValue::fromVariant(value), "smooth", 500};

    if (mode.isValid())
        data.append(QJsonValue::fromVariant(mode));

    sendCommand(method, data);
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

// NOT REVIEWED
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

// NOT REVIEWED
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

// NOT REVIEWED
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

// NOT REVIEWED
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

// NOT REVIEWED
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

        sendCommand(QString("%1set_power").arg(prefix), status);
        return;
    }

    if (name == "nightMode")
    {
        sendCommand("set_power", "on", data.toBool() ? 5 : 1);
        return;
    }

    if (m_properties.value(QString("status%1").arg(suffix)).toString() != "on")
        sendCommand(QString("%1set_power").arg(prefix), "on");

    if (name == "level")
    {
        int bright = qRound(data.toInt() * 100.0 / 255);
        sendCommand(QString("%1set_bright").arg(prefix), bright < 1 ? 1 : bright > 100 ? 100 : bright);
    }
    else if (name == "color")
    {
        QVariantList list = data.toList();
        int rgb;

        if (list.count() < 3)
            return;

        rgb = list.at(0).toInt() << 16 | list.at(1).toInt() << 8 | list.at(2).toInt();
        sendCommand(QString("%1set_rgb").arg(prefix), rgb ? rgb : 1);
    }
    else if (name == "colorTemperature")
    {
        int mired = data.toInt(), kelvin;

        if (mired < 1)
            return;

        kelvin = qRound(1000000.0 / mired);
        sendCommand(QString("%1set_ct_abx").arg(prefix), kelvin < 1700 ? 1700 : kelvin > 6500 ? 6500 : kelvin);
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
