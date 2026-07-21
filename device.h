#ifndef DEVICE_H
#define DEVICE_H

#define PING_TIMEOUT                10000
#define UNAVAILABLE_TIMEOUT         30000
#define RESET_TIMEOUT               10000

#define MULTICAST_ADDRESS           "239.255.255.250"
#define MULTICAST_PORT              1982
#define CONTROL_PORT                55443

#define COLOR_TEMPERATURE_MIN       153
#define COLOR_TEMPERATURE_MAX       370

#define BUFFER_LENGTH_LIMIT         1024





#include <QJsonArray>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

enum class Availability
{
    Unknown,
    Online,
    Offline
};

class DeviceObject;
typedef QSharedPointer <DeviceObject> Device;

class DeviceObject : public QObject
{
    Q_OBJECT

public:

    DeviceObject(const QString &address, const QString &id, bool debug);
    ~DeviceObject(void);

    inline QString id(void) { return m_id; }

    inline QString name(void) { return m_name; }
    inline void setName(const QString &value) { m_name = value; }

    inline bool ready(void) { return m_ready; }

    inline bool published(void) { return m_published; }
    inline void setPublished(void) { m_published = true; }

    inline QJsonArray exposes(void) { return m_exposes; }
    inline QJsonObject options(void) { return m_options; }

    void init(void);
    void action(const QString &name, const QVariant &data);

private:

    QString m_id, m_name;
    bool m_debug;

    QTimer *m_resetTimer, *m_updateTimer;

    QTcpSocket *m_tcp;
    QUdpSocket *m_udp;

    QHostAddress m_address;
    quint16 m_port;
    bool m_connected;

    QByteArray m_buffer;
    qint32 m_sequence, m_pending;
    bool m_background, m_ceiling, m_ready, m_published;

    Availability m_availability;
    qint64 m_lastSeen;

    QJsonArray m_exposes;
    QJsonObject m_options;

    QList <QString> m_items;
    QMap <QString, QVariant> m_properties;

    void updateAvailability(Availability availability);
    void getProperties(void);

    void sendCommand(bool bg, const QString &method, const QJsonArray &params = QJsonArray());
    void sendCommand(bool bg, const QString &method, const QVariant &value, const QVariant &mode = QVariant());







    void parseSearch(const QByteArray &datagram);
    void parseMessage(const QByteArray &message);

    void buildCapabilities(const QString &model, const QList <QString> &support);
    void addLight(bool bg, const QList <QString> &support);

    void parseProperties(const QMap <QString, QVariant> &data);
    void mapProperties(bool bg, const QMap <QString, QVariant> &data, QMap <QString, QVariant> &properties);







private slots:

    void socketError(QAbstractSocket::SocketError error);
    void socketConnected(void);
    void socketDisconnected(void);
    void readyRead(void);

    void reset(void);
    void update(void);

signals:

    void availabilityUpdated(Availability availability);
    void propertiesUpdated(const QMap <QString, QVariant> &properties);
    void capabilitiesUpdated(void);

};

inline QDebug operator << (QDebug debug, DeviceObject *device) { return debug << "device" << device->id(); }

#endif
