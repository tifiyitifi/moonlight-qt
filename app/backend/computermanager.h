#pragma once

#include "nvcomputer.h"
#include "settings/streamingpreferences.h"
#include "settings/compatfetcher.h"

#include <qmdnsengine/server.h>
#include <qmdnsengine/cache.h>
#include <qmdnsengine/browser.h>
#include <qmdnsengine/service.h>
#include <qmdnsengine/resolver.h>

#include <QThread>
#include <QReadWriteLock>
#include <QSettings>
#include <QRunnable>
#include <QTimer>
#include <QMutex>
#include <QHostInfo>
#include <QWaitCondition>

#include <utility>

class ComputerManager;

class DelayedFlushThread : public QThread
{
    Q_OBJECT

public:
    DelayedFlushThread(ComputerManager* cm)
        : m_ComputerManager(cm)
    {
        setObjectName("CM Delayed Flush Thread");
    }

    void run();

private:
    ComputerManager* m_ComputerManager;
};

class MdnsPendingComputer : public QObject
{
    Q_OBJECT

public:
    explicit MdnsPendingComputer(const QSharedPointer<QMdnsEngine::Server> server,
                                 const QMdnsEngine::Service& service)
        : m_Hostname(service.hostname()),
          m_Port(service.port()),
          m_ServerWeak(server),
          m_Resolver(nullptr)
    {
        // Start resolving
        resolve();
    }

    virtual ~MdnsPendingComputer()
    {
        delete m_Resolver;
    }

    QString hostname()
    {
        return m_Hostname;
    }

    uint16_t port()
    {
        return m_Port;
    }

private slots:
    void handleResolvedTimeout()
    {
        if (m_Addresses.isEmpty()) {
            if (m_Retries-- > 0) {
                // Try again
                qInfo() << "Resolving" << hostname() << "timed out. Retrying...";
                resolve();
            }
            else {
                qWarning() << "Giving up on resolving" << hostname() << "after repeated failures";
                cleanup();
            }
        }
        else {
            Q_ASSERT(!m_Addresses.isEmpty());
            emit resolvedHost(this, m_Addresses);
        }
    }

    void handleResolvedAddress(const QHostAddress& address)
    {
        m_Addresses.push_back(address);
    }

signals:
    void resolvedHost(MdnsPendingComputer*,QVector<QHostAddress>&);

private:
    void cleanup()
    {
        // Delete our resolver, so we're guaranteed that nothing is referencing m_Server.
        delete m_Resolver;
        m_Resolver = nullptr;

        // Now delete our strong reference that we held on behalf of m_Resolver.
        // The server may be destroyed after we make this call.
        m_Server.reset();
    }

    void resolve()
    {
        // Clean up any existing resolver object and server references
        cleanup();

        // Re-acquire a strong reference if the server still exists.
        m_Server = m_ServerWeak.toStrongRef();
        if (!m_Server) {
            return;
        }

        m_Resolver = new QMdnsEngine::Resolver(m_Server.data(), m_Hostname);
        connect(m_Resolver, &QMdnsEngine::Resolver::resolved,
                this, &MdnsPendingComputer::handleResolvedAddress);
        QTimer::singleShot(2000, this, &MdnsPendingComputer::handleResolvedTimeout);
    }

    QByteArray m_Hostname;
    uint16_t m_Port;
    QWeakPointer<QMdnsEngine::Server> m_ServerWeak;
    QSharedPointer<QMdnsEngine::Server> m_Server;
    QMdnsEngine::Resolver* m_Resolver;
    QVector<QHostAddress> m_Addresses;
    int m_Retries = 10;
};

class ComputerPollingEntry
{
public:
    ComputerPollingEntry()
        : m_ActiveThread(nullptr)
    {

    }

    virtual ~ComputerPollingEntry()
    {
        interrupt();

        // interrupt() should have taken care of this
        Q_ASSERT(m_ActiveThread == nullptr);

        for (QThread* thread : std::as_const(m_InactiveList)) {
            thread->wait();
            delete thread;
        }
    }

    bool isActive()
    {
        cleanInactiveList();

        return m_ActiveThread != nullptr;
    }

    void setActiveThread(QThread* thread)
    {
        cleanInactiveList();

        Q_ASSERT(!isActive());
        m_ActiveThread = thread;
    }

    void interrupt()
    {
        cleanInactiveList();

        if (m_ActiveThread != nullptr) {
            // Interrupt the active thread
            m_ActiveThread->requestInterruption();

            // Place it on the inactive list awaiting death
            m_InactiveList.append(m_ActiveThread);

            m_ActiveThread = nullptr;
        }
    }

private:
    void cleanInactiveList()
    {
        QMutableListIterator<QThread*> i(m_InactiveList);

        // Reap any threads that have finished
        while (i.hasNext()) {
            i.next();

            QThread* thread = i.value();
            if (thread->isFinished()) {
                delete thread;
                i.remove();
            }
        }
    }

    QThread* m_ActiveThread;
    QList<QThread*> m_InactiveList;
};

class ComputerManager : public QObject
{
    Q_OBJECT

    friend class DeferredHostDeletionTask;
    friend class PendingAddTask;
    friend class PendingPairingTask;
    friend class DelayedFlushThread;

public:
    explicit ComputerManager(StreamingPreferences* prefs);

    virtual ~ComputerManager();

    Q_INVOKABLE void startPolling();

    Q_INVOKABLE void stopPollingAsync();

    Q_INVOKABLE void addNewHostManually(QString address);

    void addNewHost(NvAddress address, bool mdns, QString name = QString(),
                    NvAddress mdnsIpv6Address = NvAddress(),
                    QVector<QHostAddress> mdnsAddresses = QVector<QHostAddress>());

    QString generatePinString();

    void pairHost(NvComputer* computer, QString pin);

    void quitRunningApp(NvComputer* computer);

    QVector<NvComputer*> getComputers();

    // computer is deleted inside this call
    void deleteHost(NvComputer* computer);

    void renameHost(NvComputer* computer, QString name);

    void setHostConnectionOptions(NvComputer* computer, quint32 allowedLinkTypes);

    void clientSideAttributeUpdated(NvComputer* computer);

signals:
    void computerStateChanged(NvComputer* computer);

    void pairingCompleted(NvComputer* computer, QString error);

    void computerAddCompleted(QVariant success, QVariant detectedPortBlocking);

    void quitAppCompleted(QVariant error);

private slots:
    void handleAboutToQuit();

    void handleComputerStateChanged(NvComputer* computer);

    void handleMdnsServiceResolved(MdnsPendingComputer* computer, QVector<QHostAddress>& addresses);

    // Completion handler for the hostname resolution started by
    // setHostConnectionOptions() and by handleReResolveHostAddresses(). `uuid`
    // rather than the host pointer because the host can be deleted while the
    // lookup is in flight, and NvComputer is not a QObject, so there is no
    // QPointer to lean on. `replaceExisting` supersedes the literals we already
    // hold instead of joining them - see resolveHostAddresses().
    void handleHostAddressResolved(QString uuid, QString name, quint16 port,
                                   QHostInfo info, bool replaceExisting);

    // Re-resolves a named host whose known addresses have stopped answering.
    // Connected to PcMonitorThread::reResolveHostAddresses, so it arrives on our
    // thread, and looks the host up by uuid rather than trusting the pointer the
    // emit handed over: deleteHost() can remove the host from the map while the
    // call is still queued.
    void handleReResolveHostAddresses(NvComputer* computer);

private:
    void saveHosts();

    void saveHost(NvComputer* computer);

    // Phase two of host deletion. DeferredHostDeletionTask does everything that
    // can block (stopping the polling thread, removing the host from the map) on
    // a worker thread, then hands the object to us so that the free itself runs
    // on our thread. That is the only correct place for it: every other pointer
    // to an NvComputer - the lookupHost callback, the QML-invoked setters, the
    // ComputerModel/AppModel slots - lives on our thread, and a free on the
    // worker would race all of them. NvComputer is not a QObject, so there is no
    // QPointer to fall back on.
    //
    // Safe to call from any thread.
    void enqueueHostDeletion(NvComputer* computer);

    // Frees everything enqueueHostDeletion() parked. Only call this on our own
    // thread. The destructor calls it too, because at exit the event loop is
    // already gone - main.cpp only waits for the thread pool after app.exec()
    // returns - so the queued drain would never be delivered.
    void drainPendingHostDeletions();

    // Resolves every non-literal address `computer` knows into
    // NvComputer::resolvedAddresses, so the connection type filter has literals
    // to classify. Asynchronous, because it runs on the GUI thread that QML
    // called setHostConnectionOptions() from, and a blocking lookup would stall
    // the UI for the length of a DNS timeout.
    //
    // Only called when a mask other than NLT_ALL is being applied: that is the
    // only time a literal is needed, so a host with no user preference never
    // pays for a lookup and keeps the address list it had before this feature
    // existed.
    //
    // `replaceExisting` is for the re-resolve path (see
    // handleReResolveHostAddresses()): the literals we already hold were
    // resolved from the same name at some point in the past and may describe an
    // address the PC has since moved away from, so a fresh answer supersedes
    // them rather than joining them. The mask-change path must NOT do that: a
    // host can be addressed by two names, and the second lookup's answer would
    // then evict the first one's, leaving whichever resolved last in the list.
    void resolveHostAddresses(NvComputer* computer, quint32 allowedLinkTypes,
                              bool replaceExisting = false);

    QHostAddress getBestGlobalAddressV6(QVector<QHostAddress>& addresses);

    void startPollingComputer(NvComputer* computer);

    StreamingPreferences* m_Prefs;
    int m_PollingRef;
    QReadWriteLock m_Lock;
    QMap<QString, NvComputer*> m_KnownHosts;
    QMap<QString, ComputerPollingEntry*> m_PollEntries;
    QHash<QString, NvComputer> m_LastSerializedHosts; // Protected by m_DelayedFlushMutex
    QSharedPointer<QMdnsEngine::Server> m_MdnsServer;
    QMdnsEngine::Browser* m_MdnsBrowser;
    QVector<MdnsPendingComputer*> m_PendingResolution;
    CompatFetcher m_CompatFetcher;
    DelayedFlushThread* m_DelayedFlushThread;
    QMutex m_DelayedFlushMutex; // Lock ordering: Must never be acquired while holding NvComputer lock
    QWaitCondition m_DelayedFlushCondition;
    bool m_NeedsDelayedFlush;
    QMutex m_PendingDeletionMutex;
    QVector<NvComputer*> m_PendingDeletion;
};
