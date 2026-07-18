#ifndef DEVICE_H
#define DEVICE_H

#define PING_TIMEOUT                10000
#define UNAVAILABLE_TIMEOUT         30000
#define RESET_TIMEOUT               10000

#define MULTICAST_ADDRESS           "239.255.255.250"
#define MULTICAST_PORT              1982
#define CONTROL_PORT                55443

#define EFFECT                      "smooth"
#define TRANSITION                  500

#define COLOR_TEMPERATURE_MIN       153
#define COLOR_TEMPERATURE_MAX       370

#define BUFFER_LENGTH_LIMIT         8192

#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
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

    void action(const QString &name, const QVariant &data);

    inline QString id(void) { return m_id; }
    inline QString name(void) { return m_name; }
    inline void setName(const QString &value) { m_name = value; }
    inline bool ready(void) { return m_ready; }
    inline bool published(void) { return m_published; }
    inline void setPublished(void) { m_published = true; }
    inline QJsonArray exposes(void) { return m_exposes; }
    inline QJsonObject options(void) { return m_options; }

    void init(void);

private:

    QString m_id, m_name;
    bool m_debug;

    QUdpSocket *m_probe;
    QTcpSocket *m_socket;

    QTimer *m_resetTimer, *m_updateTimer;

    QHostAddress m_address;
    quint16 m_port;
    bool m_connected;

    qint32 m_sequence;
    QMap <qint32, QList <QString> > m_pending;

    QByteArray m_buffer;

    Availability m_availability;
    qint64 m_lastSeen;
    bool m_ready, m_background, m_ceiling, m_published;

    QJsonArray m_exposes;
    QJsonObject m_options;

    QList <QString> m_poll;
    QMap <QString, QVariant> m_properties;

    void updateAvailability(Availability availability);
    void sendSearch(void);
    void sendCommand(const QString &method, const QJsonArray &params = QJsonArray());
    void setProperty(const QString &method, const QVariant &value);
    void getProperties(const QList <QString> &names);

    void parseSearch(const QByteArray &datagram);
    void parseMessage(const QByteArray &message);

    void buildCapabilities(const QString &model, const QList <QString> &support);
    void addLight(const QString &prefix, const QString &suffix, const QList <QString> &support);

    void parseProperties(const QMap <QString, QVariant> &data);
    void mapProperties(const QString &prefix, const QString &suffix, const QMap <QString, QVariant> &data, QMap <QString, QVariant> &properties);

    void controlLight(const QString &name, const QString &suffix, const QVariant &data);

private slots:

    void readyRead(void);

    void socketError(QAbstractSocket::SocketError error);
    void socketConnected(void);
    void socketDisconnected(void);

    void reset(void);
    void ping(void);
    void update(void);

signals:

    void availabilityUpdated(Availability availability);
    void propertiesUpdated(const QMap <QString, QVariant> &properties);
    void capabilitiesUpdated(void);

};

inline QDebug operator << (QDebug debug, DeviceObject *device) { return debug << "device" << device->id(); }

#endif
