#include "computermanager.h"
#include "boxartmanager.h"
#include "nvhttp.h"
#include "nvpairingmanager.h"
#include "netlink.h"

#include <Limelight.h>
#include <QtEndian>

#include <QThread>
#include <QThreadPool>
#include <QCoreApplication>
#include <QDateTime>
#include <QHostInfo>
#include <QPointer>
#include <QRandomGenerator>

#include <utility>

#define SER_HOSTS "hosts"
#define SER_HOSTS_BACKUP "hostsbackup"

class PcMonitorThread : public QThread
{
    Q_OBJECT

#define TRIES_BEFORE_OFFLINING 2
#define POLLS_PER_APPLIST_FETCH 10

    // How long a named host may stay unreachable before we resolve its name
    // again. The first failure is acted on immediately - a name that resolved
    // an hour ago is exactly what a host that has since changed address needs -
    // and the delay doubles from there so a host that is simply powered off
    // does not cost a DNS query every poll round forever. 15 minutes is the
    // ceiling: long enough to be free in practice, short enough that a host
    // which comes back is picked up without the user thinking to reload it.
#define NAME_RE_RESOLVE_MIN_DELAY_MS (60 * 1000)
#define NAME_RE_RESOLVE_MAX_DELAY_MS (15 * 60 * 1000)

public:
    PcMonitorThread(NvComputer* computer)
        : m_Computer(computer)
    {
        setObjectName("Polling thread for " + computer->name);
    }

private:
    bool tryPollComputer(QNetworkAccessManager* nam, NvAddress address, bool& changed)
    {
        NvHTTP http(address, 0, m_Computer->serverCert, !m_Computer->isNvidiaServerSoftware, nam);

        QString serverInfo;
        try {
            serverInfo = http.getServerInfo(NvHTTP::NvLogLevel::NVLL_NONE, true);
        } catch (...) {
            return false;
        }

        NvComputer newState(http, serverInfo);

        // Ensure the machine that responded is the one we intended to contact
        if (m_Computer->uuid != newState.uuid) {
            qInfo() << "Found unexpected PC" << newState.name << "looking for" << m_Computer->name;
            return false;
        }

        changed = m_Computer->update(newState);
        return true;
    }

    bool updateAppList(QNetworkAccessManager* nam, bool& changed)
    {
        // NvHTTP::setAddress() asserts that the address is not null, so a host
        // that we could not name a usable address for has nothing to fetch
        // here either way - report the failure instead of taking down a debug
        // build. The whole thing has to happen under one lock: the NvHTTP
        // constructor that takes an NvComputer* reads activeAddress again
        // without one, so guarding only the check would still leave the assert
        // reachable between the two. This runs on the polling thread, which
        // setHostConnectionOptions() and update() may be writing concurrently,
        // and NvAddress holds a QString, so the unlocked read could otherwise
        // land on a freed d-pointer. Take a snapshot and build from that.
        NvAddress address;
        uint16_t httpsPort;
        QSslCertificate serverCert;
        bool isNvidiaServerSoftware;
        {
            QReadLocker readLock(&m_Computer->lock);
            address = m_Computer->activeAddress;
            httpsPort = m_Computer->activeHttpsPort;
            serverCert = m_Computer->serverCert;
            isNvidiaServerSoftware = m_Computer->isNvidiaServerSoftware;

            if (address.isNull()) {
                return false;
            }
        }

        NvHTTP http(address, httpsPort, serverCert, !isNvidiaServerSoftware, nam);

        QVector<NvApp> appList;

        try {
            appList = http.getAppList();
            if (appList.isEmpty()) {
                return false;
            }
        } catch (...) {
            return false;
        }

        QWriteLocker lock(&m_Computer->lock);
        changed = m_Computer->updateAppList(appList);
        return true;
    }

    void run() override
    {
        // Reduce the power and performance impact of our
        // computer status polling while it's running.
        setPriority(QThread::LowPriority);
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
        setServiceLevel(QThread::QualityOfService::Eco);
#endif

        // Share the QNetworkAccessManager to conserve resources when polling.
        // Each instance creates a worker thread, so sharing them ensures that
        // we are not spamming a new thread for every single polling attempt.
        //
        // Since QThread inherit the priority of the current thread, this also
        // ensures that the NAM's worker thread will inherit our lower priority.
        QNetworkAccessManager nam;

        // Always fetch the applist the first time
        int pollsSinceLastAppListFetch = POLLS_PER_APPLIST_FETCH;
        while (!isInterruptionRequested()) {
            bool stateChanged = false;
            bool online = false;
            bool wasOnline = m_Computer->state == NvComputer::CS_ONLINE;
            for (int i = 0; i < (wasOnline ? TRIES_BEFORE_OFFLINING : 1) && !online; i++) {
                // Prefer the per-PC filtered list so a multi-homed host is only
                // ever *probed* over a connection type the user allows.
                // An empty list means nothing qualifies, which leaves online
                // false and takes the host CS_OFFLINE below. Returning the
                // unfiltered list instead would contact an address that is only
                // reachable over a connection type the user excluded. The
                // unfiltered path is kept for when the feature is compiled out
                // entirely.
                //
                // "Probed" is as far as this goes: which adapter the bytes
                // leave through is the OS's choice, and when several adapters
                // share a subnet the interface metric decides. The block below
                // warns when that choice is a type the user excluded.
                QVector<NvAddress> candidateAddresses = m_Computer->uniqueAddresses();
                if (NetLinkFilter::kEnabled) {
                    candidateAddresses = m_Computer->allowedAddresses();
                }

                for (auto& address : candidateAddresses) {
                    if (isInterruptionRequested()) {
                        return;
                    }

                    if (tryPollComputer(&nam, address, stateChanged)) {
                        if (!wasOnline) {
                            qInfo() << m_Computer->name << "is now online at" << m_Computer->activeAddress.toString();
                        }
                        online = true;
                        break;
                    }
                }
            }

            // Check if we failed after all retry attempts
            // Note: we don't need to acquire the read lock here,
            // because we're on the writing thread.
            if (!online && m_Computer->state != NvComputer::CS_OFFLINE) {
                qInfo() << m_Computer->name << "is now offline";
                m_Computer->state = NvComputer::CS_OFFLINE;
                stateChanged = true;
            }

            // A host addressed by name that we cannot reach may simply have moved
            // to a different address, and nothing else here can ever find that
            // out. The literals we hold were resolved from the name once, when the
            // mask was set, and a PC that keeps the same name and gets a new
            // address is ordinary (DHCP lease, a different subnet, a different
            // NIC being plugged in). We cannot re-resolve on a timer for every
            // host, but we CAN do it when a host that has a name actually fails,
            // which is the one moment the answer would change something.
            //
            // Emitted rather than resolved here because the lookup is
            // asynchronous and has to be delivered on the manager's thread, which
            // this thread has no event loop for. The pointer is only used to read
            // the uuid: the handler looks the host up again, so a host deleted
            // between the emit and the delivery is not dereferenced.
            if (NetLinkFilter::kEnabled && !online && !isInterruptionRequested() &&
                    m_Computer->hasNameAddress()) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now >= nextNameReResolveMs) {
                    nextNameReResolveMs = now + nameReResolveDelayMs;
                    nameReResolveDelayMs = qMin(nameReResolveDelayMs * 2,
                                               qint64(NAME_RE_RESOLVE_MAX_DELAY_MS));
                    emit reResolveHostAddresses(m_Computer);
                }
            }
            else if (online) {
                // Back to the front of the schedule, so the next outage is acted
                // on at once rather than inheriting the delay this host reached
                // while it was off.
                nextNameReResolveMs = 0;
                nameReResolveDelayMs = NAME_RE_RESOLVE_MIN_DELAY_MS;
            }

            // Grab the applist if it's empty or it's been long enough that we need to refresh
            pollsSinceLastAppListFetch++;
            if (m_Computer->state == NvComputer::CS_ONLINE &&
                    m_Computer->pairState == NvComputer::PS_PAIRED &&
                    (m_Computer->appList.isEmpty() || pollsSinceLastAppListFetch >= POLLS_PER_APPLIST_FETCH)) {
                // Notify prior to the app list poll since it may take a while, and we don't
                // want to delay onlining of a machine, especially if we already have a cached list.
                if (stateChanged) {
                    emit computerStateChanged(m_Computer);
                    stateChanged = false;
                }

                if (updateAppList(&nam, stateChanged)) {
                    pollsSinceLastAppListFetch = 0;
                }
            }

            if (stateChanged) {
                // Tell anyone listening that we've changed state
                emit computerStateChanged(m_Computer);
            }

            // Warn when the connection type the user allowed for this host is
            // not the one Windows will actually use. This only happens when
            // several adapters share an IP subnet, in which case the
            // destination address cannot select the adapter and the interface
            // metric decides instead. See PcSettingsView.qml for the same
            // message shown in the UI.
            if (NetLinkFilter::kEnabled && !isInterruptionRequested()) {
                bool isOnline;
                NvAddress active;
                QString name;
                {
                    // Snapshot these under one lock. setHostConnectionOptions()
                    // clears activeAddress and rewrites state from the GUI
                    // thread, and this block runs every poll round, so reading
                    // them unlocked can land on a freed d-pointer - NvAddress
                    // and name both hold a QString. getConflictingLinkDescription()
                    // takes the lock itself, so it is called after this scope
                    // rather than inside it: CopySafeReadWriteLock is not
                    // recursive. Same shape as updateAppList() above and
                    // ComputerModel::data().
                    QReadLocker readLock(&m_Computer->lock);
                    isOnline = (m_Computer->state == NvComputer::CS_ONLINE);
                    active = m_Computer->activeAddress;
                    name = m_Computer->name;
                }

                if (isOnline) {
                    const QString conflict = m_Computer->getConflictingLinkDescription();
                    if (!conflict.isEmpty() && active != lastConflictAddress) {
                        lastConflictAddress = active;
                        qWarning().nospace().noquote() << name
                                                       << "is online, but Windows sends the traffic through"
                                                       << conflict << "despite the connection types allowed for"
                                                       << "this PC. Lower that adapter's interface metric,"
                                                       << "or put the adapters on separate subnets.";
                    }
                    else if (conflict.isEmpty()) {
                        lastConflictAddress = NvAddress();
                    }
                }
            }

            // Wait a bit to poll again, but do it in 100 ms chunks
            // so we can be interrupted reasonably quickly.
            // FIXME: QWaitCondition would be better.
            for (int i = 0; i < 30 && !isInterruptionRequested(); i++) {
                QThread::msleep(100);
            }
        }
    }

signals:
   void computerStateChanged(NvComputer* computer);

   // This host is unreachable and carries a name, so resolve it again. Queued
   // to ComputerManager, which owns the lookup and the host map.
   void reResolveHostAddresses(NvComputer* computer);

private:
    NvComputer* m_Computer;

    // Used to avoid repeating the same routing conflict warning every 3 seconds
    NvAddress lastConflictAddress;

    // Backoff state for the name re-resolve above. nextNameReResolveMs is an
    // absolute deadline rather than a timer because this loop has no event loop
    // to fire anything from; 0 means "eligible now", which is what makes the
    // first failure act immediately.
    qint64 nextNameReResolveMs = 0;
    qint64 nameReResolveDelayMs = NAME_RE_RESOLVE_MIN_DELAY_MS;
};

ComputerManager::ComputerManager(StreamingPreferences* prefs)
    : m_Prefs(prefs),
      m_PollingRef(0),
      m_MdnsBrowser(nullptr),
      m_CompatFetcher(nullptr),
      m_NeedsDelayedFlush(false)
{
    QSettings settings;

    // If there's a hosts backup copy, we must have failed to commit
    // a previous update before exiting. Restore the backup now.
    int hosts = settings.beginReadArray(SER_HOSTS_BACKUP);
    if (hosts == 0) {
        // If there's no host backup, read from the primary location.
        settings.endArray();
        hosts = settings.beginReadArray(SER_HOSTS);
    }

    // Inflate our hosts from QSettings
    for (int i = 0; i < hosts; i++) {
        settings.setArrayIndex(i);
        NvComputer* computer = new NvComputer(settings);
        m_KnownHosts[computer->uuid] = computer;
        m_LastSerializedHosts[computer->uuid] = *computer;
    }
    settings.endArray();

    // Fetch latest compatibility data asynchronously
    m_CompatFetcher.start();

    // Start the delayed flush thread to handle saveHosts() calls
    m_DelayedFlushThread = new DelayedFlushThread(this);
    m_DelayedFlushThread->start();

    // To quit in a timely manner, we must block additional requests
    // after we receive the aboutToQuit() signal. This is necessary
    // because NvHTTP uses aboutToQuit() to abort requests in progress
    // while quitting, however this is a one time signal - additional
    // requests would not be aborted and block termination.
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ComputerManager::handleAboutToQuit);
}

ComputerManager::~ComputerManager()
{
    // Stop the delayed flush thread before acquiring the lock in write mode
    // to avoid deadlocking with a flush that needs the lock in read mode.
    {
        // Wake the delayed flush thread
        m_DelayedFlushThread->requestInterruption();
        m_DelayedFlushCondition.wakeOne();

        // Wait for it to terminate (and finish any pending flush)
        m_DelayedFlushThread->wait();
        delete m_DelayedFlushThread;

        // Delayed flushes should have completed by now
        Q_ASSERT(!m_NeedsDelayedFlush);
    }

    QWriteLocker lock(&m_Lock);

    // Delete machines that haven't been resolved yet
    while (!m_PendingResolution.isEmpty()) {
        MdnsPendingComputer* computer = m_PendingResolution.first();
        delete computer;
        m_PendingResolution.removeFirst();
    }

    // Delete the browser to stop discovery
    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;

    // Interrupt polling
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }

    // Delete all polling entries (and associated threads)
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        delete entry;
    }

    // Destroy all NvComputer objects now that polling is halted
    for (NvComputer* computer : std::as_const(m_KnownHosts)) {
        delete computer;
    }

    // Hosts handed to us by DeferredHostDeletionTask are not in m_KnownHosts, so
    // the loop above never sees them. The event loop is already stopped by the
    // time we get here, so their queued drain will not be delivered.
    drainPendingHostDeletions();
}

void DelayedFlushThread::run() {
    for (;;) {
        // Wait for a delayed flush request or an interruption
        {
            QMutexLocker locker(&m_ComputerManager->m_DelayedFlushMutex);

            while (!QThread::currentThread()->isInterruptionRequested() && !m_ComputerManager->m_NeedsDelayedFlush) {
                m_ComputerManager->m_DelayedFlushCondition.wait(&m_ComputerManager->m_DelayedFlushMutex);
            }

            // Bail without flushing if we woke up for an interruption alone.
            // If we have both an interruption and a flush request, do the flush.
            if (!m_ComputerManager->m_NeedsDelayedFlush) {
                Q_ASSERT(QThread::currentThread()->isInterruptionRequested());
                break;
            }

            // Reset the delayed flush flag to ensure any racing saveHosts() call will set it again
            m_ComputerManager->m_NeedsDelayedFlush = false;

            // Update the last serialized hosts map under the delayed flush mutex
            m_ComputerManager->m_LastSerializedHosts.clear();
            for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                // Copy the current state of the NvComputer to allow us to check later if we need
                // to serialize it again when attribute updates occur.
                QReadLocker computerLock(&computer->lock);
                m_ComputerManager->m_LastSerializedHosts[computer->uuid] = *computer;
            }
        }

        // Perform the flush
        {
            QSettings settings;

            // First, write to the backup location
            settings.beginWriteArray(SER_HOSTS_BACKUP);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, false);
                }
            }
            settings.endArray();

            // Next, write to the primary location
            settings.remove(SER_HOSTS);
            settings.beginWriteArray(SER_HOSTS);
            {
                QReadLocker lock(&m_ComputerManager->m_Lock);
                int i = 0;
                for (const NvComputer* computer : std::as_const(m_ComputerManager->m_KnownHosts)) {
                    settings.setArrayIndex(i++);
                    computer->serialize(settings, true);
                }
            }
            settings.endArray();

            // Finally, delete the backup copy
            settings.remove(SER_HOSTS_BACKUP);
        }
    }
}

void ComputerManager::saveHosts()
{
    Q_ASSERT(m_DelayedFlushThread != nullptr && m_DelayedFlushThread->isRunning());

    // Punt to a worker thread because QSettings on macOS can take ages (> 500 ms)
    // to persist our host list to disk (especially when a host has a bunch of apps).
    QMutexLocker locker(&m_DelayedFlushMutex);
    m_NeedsDelayedFlush = true;
    m_DelayedFlushCondition.wakeOne();
}

QHostAddress ComputerManager::getBestGlobalAddressV6(QVector<QHostAddress> &addresses)
{
    for (const QHostAddress& address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            if (address.isInSubnet(QHostAddress("fe80::"), 10)) {
                // Link-local
                continue;
            }

            if (address.isInSubnet(QHostAddress("fec0::"), 10)) {
                qInfo() << "Ignoring site-local address:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("fc00::"), 7)) {
                qInfo() << "Ignoring ULA:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("2002::"), 16)) {
                qInfo() << "Ignoring 6to4 address:" << address;
                continue;
            }

            if (address.isInSubnet(QHostAddress("2001::"), 32)) {
                qInfo() << "Ignoring Teredo address:" << address;
                continue;
            }

            return address;
        }
    }

    return QHostAddress();
}

void ComputerManager::startPolling()
{
    QWriteLocker lock(&m_Lock);

    if (++m_PollingRef > 1) {
        return;
    }

    if (m_Prefs->enableMdns) {
        // Start an MDNS query for GameStream hosts
        m_MdnsServer.reset(new QMdnsEngine::Server());
        m_MdnsBrowser = new QMdnsEngine::Browser(m_MdnsServer.data(), "_nvstream._tcp.local.");
        connect(m_MdnsBrowser, &QMdnsEngine::Browser::serviceAdded,
                this, [this](const QMdnsEngine::Service& service) {
            qInfo() << "Discovered mDNS host:" << service.hostname();

            MdnsPendingComputer* pendingComputer = new MdnsPendingComputer(m_MdnsServer, service);
            connect(pendingComputer, &MdnsPendingComputer::resolvedHost,
                    this, &ComputerManager::handleMdnsServiceResolved);
            m_PendingResolution.append(pendingComputer);
        });
    }
    else {
        qWarning() << "mDNS is disabled by user preference";
    }

    // Start polling threads for each known host
    QMapIterator<QString, NvComputer*> i(m_KnownHosts);
    while (i.hasNext()) {
        i.next();
        startPollingComputer(i.value());
    }
}

// Must hold m_Lock for write
void ComputerManager::startPollingComputer(NvComputer* computer)
{
    if (m_PollingRef == 0) {
        return;
    }

    ComputerPollingEntry* pollingEntry;

    if (!m_PollEntries.contains(computer->uuid)) {
        pollingEntry = m_PollEntries[computer->uuid] = new ComputerPollingEntry();
    }
    else {
        pollingEntry = m_PollEntries[computer->uuid];
    }

    if (!pollingEntry->isActive()) {
        PcMonitorThread* thread = new PcMonitorThread(computer);
        connect(thread, &PcMonitorThread::computerStateChanged,
                this, &ComputerManager::handleComputerStateChanged);

        // Queued, not direct: the emit happens on the polling thread, and the
        // lookup it asks for delivers its result through the manager's event
        // loop, so the manager's thread is the only place this can run.
        connect(thread, &PcMonitorThread::reResolveHostAddresses,
                this, &ComputerManager::handleReResolveHostAddresses);
        pollingEntry->setActiveThread(thread);
        thread->start();
    }
}

void ComputerManager::handleMdnsServiceResolved(MdnsPendingComputer* computer,
                                                QVector<QHostAddress>& addresses)
{
    QHostAddress v6Global = getBestGlobalAddressV6(addresses);
    bool added = false;

    // Remember every address this host advertises. A multi-homed host (for
    // example a laptop with both Ethernet and WiFi) answers with one A record
    // per interface, and keeping only the first one made it impossible to
    // fall back to a different NIC later on.
    QVector<QHostAddress> mdnsAddresses;
    if (NetLinkFilter::kEnabled) {
        for (const QHostAddress& address : std::as_const(addresses)) {
            mdnsAddresses.append(address);
        }
    }

    // Add the host using the IPv4 address
    for (const QHostAddress& address : std::as_const(addresses)) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol) {
            // NB: We don't just call addNewHost() here with v6Global because the IPv6
            // address may not be reachable (if the user hasn't installed the IPv6 helper yet
            // or if this host lacks outbound IPv6 capability). We want to add IPv6 even if
            // it's not currently reachable.
            addNewHost(NvAddress(address, computer->port()),
                       true, computer->hostname(),
                       NvAddress(v6Global, computer->port()),
                       mdnsAddresses);
            added = true;
            break;
        }
    }

    if (!added) {
        // If we get here, there wasn't an IPv4 address so we'll do it v6-only
        for (const QHostAddress& address : std::as_const(addresses)) {
            if (address.protocol() == QAbstractSocket::IPv6Protocol) {
                // Use a link-local or site-local address for the "local address"
                if (address.isInSubnet(QHostAddress("fe80::"), 10) ||
                        address.isInSubnet(QHostAddress("fec0::"), 10) ||
                        address.isInSubnet(QHostAddress("fc00::"), 7)) {
                    addNewHost(NvAddress(address, computer->port()),
                               true, computer->hostname(),
                               NvAddress(v6Global, computer->port()),
                               mdnsAddresses);
                    break;
                }
            }
        }
    }

    m_PendingResolution.removeOne(computer);
    computer->deleteLater();
}

void ComputerManager::saveHost(NvComputer *computer)
{
    // If no serializable properties changed, don't bother saving hosts
    QMutexLocker lock(&m_DelayedFlushMutex);
    QReadLocker computerLock(&computer->lock);
    if (!m_LastSerializedHosts.value(computer->uuid).isEqualSerialized(*computer)) {
        // Queue a request for a delayed flush to QSettings outside of the lock
        computerLock.unlock();
        lock.unlock();
        saveHosts();
    }
}

void ComputerManager::handleComputerStateChanged(NvComputer* computer)
{
    emit computerStateChanged(computer);

    if (computer->pendingQuit && computer->currentGameId == 0) {
        computer->pendingQuit = false;
        emit quitAppCompleted(QVariant());
    }

    // Save updates to this host
    saveHost(computer);
}

QVector<NvComputer*> ComputerManager::getComputers()
{
    QReadLocker lock(&m_Lock);

    // Return a sorted host list
    auto hosts = QVector<NvComputer*>::fromList(m_KnownHosts.values());
    std::stable_sort(hosts.begin(), hosts.end(), [](const NvComputer* host1, const NvComputer* host2) {
        return host1->name.toLower() < host2->name.toLower();
    });
    return hosts;
}

class DeferredHostDeletionTask : public QRunnable
{
public:
    DeferredHostDeletionTask(ComputerManager* cm, NvComputer* computer)
        : m_Computer(computer),
          m_ComputerManager(cm) {}

    void run()
    {
        // QPointer, not a raw pointer: main.cpp only waits for the thread pool
        // after app.exec() returns, by which point the QML engine - and with it
        // the ComputerManager singleton it owns - may already be gone. Nothing
        // below may dereference the manager unconditionally.
        ComputerManager* computerManager = m_ComputerManager;
        if (computerManager == nullptr) {
            // The manager is being destroyed, so the host is already unreachable
            // through it and only this task can still reference it. Free it here.
            delete m_Computer;
            return;
        }

        ComputerPollingEntry* pollingEntry;

        // Only do the minimum amount of work while holding the writer lock.
        // We must release it before calling saveHosts().
        {
            QWriteLocker lock(&computerManager->m_Lock);

            pollingEntry = computerManager->m_PollEntries.take(m_Computer->uuid);

            computerManager->m_KnownHosts.remove(m_Computer->uuid);
        }

        // Persist the new host list with this computer deleted
        computerManager->saveHosts();

        // Delete the polling entry first. This will stop all polling threads too.
        delete pollingEntry;

        // Delete cached box art
        BoxArtManager::deleteBoxArt(m_Computer);

        // Finally, hand the computer itself over for deletion. This must be done
        // last because the polling thread might have been using it, but the free
        // itself belongs on the manager's thread - see enqueueHostDeletion().
        computerManager->enqueueHostDeletion(m_Computer);
    }

private:
    NvComputer* m_Computer;
    QPointer<ComputerManager> m_ComputerManager;
};

void ComputerManager::deleteHost(NvComputer* computer)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for the polling thread to die
    QThreadPool::globalInstance()->start(new DeferredHostDeletionTask(this, computer));
}

void ComputerManager::enqueueHostDeletion(NvComputer* computer)
{
    if (QThread::currentThread() == thread()) {
        delete computer;
        return;
    }

    {
        QMutexLocker locker(&m_PendingDeletionMutex);
        m_PendingDeletion.append(computer);
    }

    // The context object is this manager, so if it is destroyed first QObject
    // simply drops the queued event and the host is freed by the destructor
    // instead. Capturing this in a bare context-less functor would not.
    QMetaObject::invokeMethod(this, [this] { drainPendingHostDeletions(); },
                               Qt::QueuedConnection);
}

void ComputerManager::drainPendingHostDeletions()
{
    Q_ASSERT(QThread::currentThread() == thread());

    QVector<NvComputer*> pending;
    {
        QMutexLocker locker(&m_PendingDeletionMutex);
        pending.swap(m_PendingDeletion);
    }

    for (NvComputer* computer : std::as_const(pending)) {
        delete computer;
    }
}

void ComputerManager::renameHost(NvComputer* computer, QString name)
{
    {
        QWriteLocker lock(&computer->lock);

        computer->name = name;
        computer->hasCustomName = true;
    }

    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::clientSideAttributeUpdated(NvComputer* computer)
{
    // Notify the UI of the state change
    handleComputerStateChanged(computer);
}

void ComputerManager::setHostConnectionOptions(NvComputer* computer, quint32 allowedLinkTypes)
{
    // The UI that reaches this is hidden when the filter is compiled out (see
    // ComputerModel::netLinkFilterEnabled), so getting here means a stray
    // caller. Warn rather than returning silently: writing a mask that nothing
    // reads is exactly the stale state that would resurface the moment the
    // flag is flipped back on.
    if (!NetLinkFilter::kEnabled) {
        qWarning() << "Ignoring connection type mask for" << computer->name
                   << "because NetLinkFilter::kEnabled is false.";
        return;
    }

    // The UI refuses to clear every type, but a hand-edited settings file
    // could still ask for it, so we guard here as well rather than letting a
    // single host become permanently unreachable. Unknown bits are dropped for
    // the same reason.
    allowedLinkTypes &= quint32(NLT_ALL);
    if (allowedLinkTypes == 0) {
        qWarning() << "Refusing to disable every connection type for" << computer->name
                   << "because that would make it unreachable.";
        allowedLinkTypes = NLT_ALL;
    }

    {
        QWriteLocker lock(&computer->lock);

        if (computer->allowedLinkTypes == allowedLinkTypes) {
            return;
        }

        computer->allowedLinkTypes = allowedLinkTypes;

        // Allow the fallback warning to be logged again for the new selection
        computer->linkFilterFallbackLogged = false;

        if (allowedLinkTypes == NLT_ALL) {
            // NLT_ALL filters nothing, so the literals we resolved to make a
            // hostname filterable are dead weight. Dropping them here is what
            // keeps a host the user has expressed no preference for on the exact
            // address list it had before this feature existed - extra candidates
            // would still be probed, costing a timeout each per poll round.
            computer->resolvedAddresses.clear();
        }

        // Force the poller to re-evaluate. Without this, uniqueAddresses()
        // would keep returning the old address first and nothing would change.
        computer->activeAddress = NvAddress();
        computer->state = NvComputer::CS_UNKNOWN;
    }

    qInfo() << qPrintable(computer->name) << "now allows connection types"
            << QString::number(allowedLinkTypes, 16);

    // Persist outside of the computer lock
    saveHost(computer);

    // Deliberately no polling restart here. ComputerPollingEntry::interrupt()
    // only sets the interruption flag, so a restart would leave a second
    // PcMonitorThread on this host while the first is still winding down inside
    // a network request - up to 2s per candidate address with
    // FAST_FAIL_TIMEOUT_MS, or 5s for an applist fetch - and the two would poll
    // and write the same host concurrently for as long as the first takes to
    // notice.
    //
    // Nothing needs restarting anyway: clearing activeAddress above is enough.
    // allowedAddresses() re-reads allowedLinkTypes under the computer lock on
    // every poll, and the cleared address is dropped by uniqueAddresses(), so
    // the very next round rebuilds the candidate list from the new mask and
    // update() latches whatever address survives the filter. Clearing state to
    // CS_UNKNOWN also drops us to a single try instead of TRIES_BEFORE_OFFLINING,
    // so the host comes back sooner than it would have. If the new mask admits
    // no address at all, that round simply finds an empty candidate list and
    // takes the host CS_OFFLINE, which is the intended reading of the filter.
    //
    // Known gap, deliberately not fixed by this feature. The two writes above
    // are taken under the computer lock, but PcMonitorThread::run() reads state,
    // name, pairState and appList with no lock at all - the comment at its own
    // CS_OFFLINE store claims the poller is the only writer of this host, which
    // stopped being true well before this feature: renameHost() has always
    // written name from the GUI thread, and PendingAddTask::run() folds a polled
    // computer in from a QThreadPool thread via update(), which writes
    // activeAddress and state under that lock. The writes above therefore make
    // this function one more writer of state and activeAddress from the GUI
    // thread: they widen a pre-existing data race rather than create one, and an
    // unsynchronized read of the QString inside NvAddress can still land on an
    // already-freed d-pointer. The reads this feature did add - the conflict
    // warning block in run() - are snapshotted, but the older ones at the top of
    // the loop, and the serverCert/isNvidiaServerSoftware read in
    // tryPollComputer(), are not. Closing those means a short snapshot scope at
    // each site; it cannot be done by wrapping the loop body, since
    // CopySafeReadWriteLock is not recursive, uniqueAddresses() and
    // allowedAddresses() already lock internally, and update() takes the write
    // lock from inside the loop. That is a separate change.

    // The one thing the next poll round cannot sort out on its own is a host the
    // user added by NAME. netRoutesTo() needs a literal, so until the name
    // resolves to one the mask has nothing to judge it against - which
    // allowedAddresses() reads as "keep the name", not "exclude the host", so
    // the poller still probes the name and the host is not stranded after all.
    // Resolve it into literals now, before that round lands, so the filter has
    // something to judge on the next one: once a literal does exist, the name
    // is dropped, and if the mask then excludes every literal the host is taken
    // offline on purpose (which getMaskExcludedLinkTypes() reports, and
    // getUnclassifiableAddress() reports the window before it).
    //
    // replaceExisting stays false here: a host can carry two names, and clearing
    // the list on one lookup's answer would evict whatever the other resolved to.
    resolveHostAddresses(computer, allowedLinkTypes);

    // Tell the UI the new state
    handleComputerStateChanged(computer);
}

void ComputerManager::handleReResolveHostAddresses(NvComputer* computer)
{
    if (!NetLinkFilter::kEnabled) {
        return;
    }

    // Look the host up again rather than trusting the pointer the polling thread
    // emitted. deleteHost() takes the polling entry out of m_PollEntries and
    // joins the thread from a thread pool task, so between the emit and this
    // delivery the host can be gone from the map - and since NvComputer is not a
    // QObject there is no QPointer to fall back on. The uuid is read under the
    // computer lock, which is still held here by construction: the thread cannot
    // be joined, and the computer freed, before the queued call is delivered.
    QString uuid;
    quint32 allowedLinkTypes;
    {
        QReadLocker lock(&computer->lock);
        uuid = computer->uuid;
        allowedLinkTypes = computer->allowedLinkTypes;
    }

    NvComputer* host;
    {
        QReadLocker lock(&m_Lock);
        host = m_KnownHosts.value(uuid, nullptr);
    }

    if (host == nullptr) {
        return;
    }

    // Replacing is the point of this path: the literals we hold were resolved
    // from this name at some earlier moment, and a host that kept its name while
    // changing address would otherwise be probed against an address it no longer
    // has - forever, since nothing else re-resolves it.
    resolveHostAddresses(host, allowedLinkTypes, true);
}

void ComputerManager::resolveHostAddresses(NvComputer* computer, quint32 allowedLinkTypes,
                                           bool replaceExisting)
{
    if (!NetLinkFilter::kEnabled || allowedLinkTypes == NLT_ALL) {
        return;
    }

    QVector<NvAddress> named;
    QString uuid;
    {
        QReadLocker lock(&computer->lock);

        uuid = computer->uuid;

        // manualAddress is the one addNewHostManually() fills from whatever the
        // user typed, so it is the one that can be a name. activeAddress can
        // hold a name too, but only transiently: a hostname is latched while
        // the mask is still NLT_ALL, and the first round after a mask is
        // narrowed replaces it with a resolved literal.
        if (!computer->manualAddress.isNull()) {
            named.append(computer->manualAddress);
        }
        if (!computer->activeAddress.isNull() && computer->activeAddress != computer->manualAddress) {
            named.append(computer->activeAddress);
        }
    }

    for (const NvAddress& address : std::as_const(named)) {
        if (netAddressIsLiteral(address.address())) {
            continue;
        }

        // Asynchronous because this runs on the GUI thread that QML invoked
        // setHostConnectionOptions() from, and a blocking lookup would stall the
        // UI for the length of a DNS timeout.
        //
        // The receiver is this, so the callback is dropped if the manager goes
        // away mid-lookup. It is not the QNetworkAccessManager's, so a proxy
        // configured for HTTP traffic cannot intercept the lookup - we want the
        // system resolver, since the answer decides which adapter we use.
        const QString name = address.address();
        const quint16 port = address.port();
        QHostInfo::lookupHost(name, this, [this, uuid, name, port, replaceExisting](const QHostInfo& info) {
            handleHostAddressResolved(uuid, name, port, info, replaceExisting);
        });
    }
}

void ComputerManager::handleHostAddressResolved(QString uuid, QString name, quint16 port,
                                               QHostInfo info, bool replaceExisting)
{
    if (info.error() != QHostInfo::NoError) {
        // Not fatal and not worth retrying on a timer beyond the polling
        // thread's own backoff: we keep whatever literals we already had, so a
        // transient DNS failure cannot take a reachable host offline. When there
        // are none, allowedAddresses() keeps the hostname anyway and
        // getUnclassifiableAddress() tells the user the mask cannot be enforced
        // for this PC.
        qWarning() << "Unable to resolve" << name << "for PC" << uuid << ":"
                   << info.errorString();
        return;
    }

    if (info.addresses().isEmpty()) {
        return;
    }

    // The host can be deleted while the lookup is in flight, so the callback above
    // captured the uuid rather than the pointer: an in-flight lookup must not hold
    // a reference the user can invalidate. Re-resolving by uuid is still required -
    // a host deleted in the meantime is simply not in the map anymore.
    //
    // The pointer it returns is safe to use from here on. This runs on the
    // manager's thread, and that is the only thread an NvComputer is ever freed on
    // (see enqueueHostDeletion()), so no worker can pull the object out from under
    // us between the lookup and the writes below. That is the reason the lock may
    // be released before use - not that m_Lock protects lifetime, which it never
    // did: it guards the map, not what the map points at.
    NvComputer* computer;
    {
        QReadLocker lock(&m_Lock);
        computer = m_KnownHosts.value(uuid, nullptr);
    }

    if (computer == nullptr) {
        return;
    }

    bool changed = false;
    int replacedCount = 0;
    {
        QWriteLocker lock(&computer->lock);

        // A mask that went back to NLT_ALL while we were waiting needs no
        // literals, and setHostConnectionOptions() has already cleared them.
        // Re-adding them would hand a host that is supposed to filter nothing a
        // longer candidate list than it had before this feature existed.
        if (computer->allowedLinkTypes == NLT_ALL) {
            return;
        }

        // Every address the name resolves to, not just the first. A name can
        // legitimately answer on more than one of the host's addresses, and
        // each one is classified on its own by allowedAddresses() - which is how
        // a hostname spanning both a wired and a wireless address gets reduced
        // to the wired one.
        QVector<NvAddress> resolved;
        resolved.reserve(info.addresses().count());
        for (const QHostAddress& address : info.addresses()) {
            resolved.append(NvAddress(address, port));
        }

        // A replacement is compared against the whole list before being applied.
        // The poller rebuilds its candidate list from the lock on every round, so
        // a rewrite that changes nothing would cost a saveHost() and a model
        // reset for no reason at all - and the re-resolve path runs on a backoff
        // timer, so "nothing changed" is the overwhelmingly common outcome for a
        // host that is merely powered off.
        if (replaceExisting) {
            if (computer->resolvedAddresses != resolved) {
                // Counted before the swap, so the log line below can say what it
                // replaced rather than what it replaced it with.
                replacedCount = computer->resolvedAddresses.count();
                computer->resolvedAddresses = resolved;
                changed = true;
            }
        }
        else {
            for (const NvAddress& candidate : std::as_const(resolved)) {
                if (!computer->resolvedAddresses.contains(candidate)) {
                    computer->resolvedAddresses.append(candidate);
                    changed = true;
                }
            }
        }
    }

    if (!changed) {
        return;
    }

    if (replaceExisting) {
        // Worth saying out loud, because the addresses a name resolved to
        // changing is a real event: the old ones were reachable a moment ago, and
        // this is the only path that can produce new ones after the mask was set.
        qInfo() << "Re-resolved" << name << "for PC" << uuid << "to" << info.addresses().count()
                << "address(es), replacing" << replacedCount << "previous literal(s).";
    }
    else {
        qInfo() << "Resolved" << name << "for PC" << uuid << "to" << info.addresses().count()
                << "address(es); the connection type filter can now judge them.";
    }

    // Persist outside the computer lock, then refresh the UI. No polling
    // restart: the poller rebuilds its candidate list from the lock on every
    // round, so it picks the literals up within one poll interval on its own.
    // handleComputerStateChanged() ends in saveHost(), so the persist happens
    // there and does not need its own call.
    handleComputerStateChanged(computer);
}

void ComputerManager::handleAboutToQuit()
{
    QReadLocker lock(&m_Lock);

    // Interrupt polling threads immediately, so they
    // avoid making additional requests while quitting
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }
}

class PendingPairingTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingPairingTask(ComputerManager* computerManager, NvComputer* computer, QString pin)
        : m_ComputerManager(computerManager),
          m_Computer(computer),
          m_Pin(pin)
    {
        connect(this, &PendingPairingTask::pairingCompleted,
                computerManager, &ComputerManager::pairingCompleted);
    }

signals:
    void pairingCompleted(NvComputer* computer, QString error);

private:
    void run()
    {
        NvPairingManager pairingManager(m_Computer);

        try {
           NvPairingManager::PairState result = pairingManager.pair(m_Computer->appVersion, m_Pin, m_Computer->serverCert);
           switch (result)
           {
           case NvPairingManager::PairState::PIN_WRONG:
               emit pairingCompleted(m_Computer, tr("The PIN from the PC didn't match. Please try again."));
               break;
           case NvPairingManager::PairState::FAILED:
               if (m_Computer->currentGameId != 0) {
                   emit pairingCompleted(m_Computer, tr("You cannot pair while a previous session is still running on the host PC. Quit any running games or reboot the host PC, then try pairing again."));
               }
               else {
                   emit pairingCompleted(m_Computer, tr("Pairing failed. Please try again."));
               }
               break;
           case NvPairingManager::PairState::ALREADY_IN_PROGRESS:
               emit pairingCompleted(m_Computer, tr("Another pairing attempt is already in progress."));
               break;
           case NvPairingManager::PairState::PAIRED:
               // Persist the newly pinned server certificate for this host
               m_ComputerManager->saveHost(m_Computer);

               emit pairingCompleted(m_Computer, nullptr);
               break;
           }
        } catch (const GfeHttpResponseException& e) {
            emit pairingCompleted(m_Computer, tr("GeForce Experience returned error: %1").arg(e.toQString()));
        } catch (const QtNetworkReplyException& e) {
            emit pairingCompleted(m_Computer, e.toQString());
        }
    }

    ComputerManager* m_ComputerManager;
    NvComputer* m_Computer;
    QString m_Pin;
};

void ComputerManager::pairHost(NvComputer* computer, QString pin)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for pairing to complete
    PendingPairingTask* pairing = new PendingPairingTask(this, computer, pin);
    QThreadPool::globalInstance()->start(pairing);
}

class PendingQuitTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingQuitTask(ComputerManager* computerManager, NvComputer* computer)
        : m_Computer(computer)
    {
        connect(this, &PendingQuitTask::quitAppFailed,
                computerManager, &ComputerManager::quitAppCompleted);
    }

signals:
    void quitAppFailed(QString error);

private:
    void run()
    {
        NvHTTP http(m_Computer);

        try {
            if (m_Computer->currentGameId != 0) {
                http.quitApp();
            }
        } catch (const GfeHttpResponseException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            if (e.getStatusCode() == 599) {
                // 599 is a special code we make a custom message for
                emit quitAppFailed(tr("The running game wasn't started by this PC. "
                                      "You must quit the game on the host PC manually or use the device that originally started the game."));
            }
            else {
                emit quitAppFailed(e.toQString());
            }
        } catch (const QtNetworkReplyException& e) {
            {
                QWriteLocker lock(&m_Computer->lock);
                m_Computer->pendingQuit = false;
            }
            emit quitAppFailed(e.toQString());
        }
    }

    NvComputer* m_Computer;
};

void ComputerManager::quitRunningApp(NvComputer* computer)
{
    QWriteLocker lock(&computer->lock);
    computer->pendingQuit = true;

    PendingQuitTask* quit = new PendingQuitTask(this, computer);
    QThreadPool::globalInstance()->start(quit);
}

void ComputerManager::stopPollingAsync()
{
    QWriteLocker lock(&m_Lock);

    Q_ASSERT(m_PollingRef > 0);
    if (--m_PollingRef > 0) {
        return;
    }

    // Delete machines that haven't been resolved yet
    while (!m_PendingResolution.isEmpty()) {
        MdnsPendingComputer* computer = m_PendingResolution.first();
        computer->deleteLater();
        m_PendingResolution.removeFirst();
    }

    // Delete the browser and server to stop discovery and refresh polling
    delete m_MdnsBrowser;
    m_MdnsBrowser = nullptr;
    m_MdnsServer.reset();

    // Interrupt all threads, but don't wait for them to terminate
    for (ComputerPollingEntry* entry : std::as_const(m_PollEntries)) {
        entry->interrupt();
    }
}

void ComputerManager::addNewHostManually(QString address)
{
    QUrl url = QUrl::fromUserInput("moonlight://" + address);
    if (url.isValid() && !url.host().isEmpty() && url.scheme() == "moonlight") {
        // If there wasn't a port specified, use the default
        addNewHost(NvAddress(url.host(), url.port(DEFAULT_HTTP_PORT)), false);
    }
    else if (QHostAddress(address).protocol() == QAbstractSocket::IPv6Protocol) {
        // The user specified an IPv6 literal without URL escaping, so use the default port
        addNewHost(NvAddress(address, DEFAULT_HTTP_PORT), false);
    }
    else {
        emit computerAddCompleted(false, false);
    }
}

class PendingAddTask : public QObject, public QRunnable
{
    Q_OBJECT

public:
    PendingAddTask(ComputerManager* computerManager, QString name, NvAddress address,
                   NvAddress mdnsIpv6Address, QVector<QHostAddress> mdnsAddresses, bool mdns)
        : m_ComputerManager(computerManager),
          m_Name(name),
          m_Address(address),
          m_MdnsIpv6Address(mdnsIpv6Address),
          m_MdnsAddresses(mdnsAddresses),
          m_Mdns(mdns),
          m_AboutToQuit(false)
    {
        connect(this, &PendingAddTask::computerAddCompleted,
                computerManager, &ComputerManager::computerAddCompleted);
        connect(this, &PendingAddTask::computerStateChanged,
                computerManager, &ComputerManager::handleComputerStateChanged);
        connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                this, &PendingAddTask::handleAboutToQuit);
    }

signals:
    void computerAddCompleted(QVariant success, QVariant detectedPortBlocking);

    void computerStateChanged(NvComputer* computer);

private:
    void handleAboutToQuit()
    {
        m_AboutToQuit = true;
    }

    QString fetchServerInfo(NvHTTP& http)
    {
        QString serverInfo;

        // Do nothing if we're quitting
        if (m_AboutToQuit) {
            return QString();
        }

        try {
            // There's a race condition between GameStream servers reporting presence over
            // mDNS and the HTTPS server being ready to respond to our queries. To work
            // around this issue, we will issue the request again after a few seconds if
            // we see a ServiceUnavailableError error.
            try {
                serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
            } catch (const QtNetworkReplyException& e) {
                if (e.getError() == QNetworkReply::ServiceUnavailableError) {
                    qWarning() << "Retrying request in 5 seconds after ServiceUnavailableError";
                    QThread::sleep(5);
                    serverInfo = http.getServerInfo(NvHTTP::NVLL_VERBOSE);
                    qInfo() << "Retry successful";
                }
                else {
                    // Rethrow other errors
                    throw e;
                }
            }
            return serverInfo;
        } catch (...) {
            if (!m_Mdns) {
                unsigned int portTestResult;

                if (m_ComputerManager->m_Prefs->detectNetworkBlocking) {
                    // We failed to connect to the specified PC. Let's test to make sure this network
                    // isn't blocking Moonlight, so we can tell the user about it.
                    portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443,
                                                              ML_PORT_FLAG_TCP_47984 | ML_PORT_FLAG_TCP_47989);
                }
                else {
                    portTestResult = 0;
                }

                emit computerAddCompleted(false, portTestResult != 0 && portTestResult != ML_TEST_RESULT_INCONCLUSIVE);
            }
            return QString();
        }
    }

    void run()
    {
        // Use the placeholder UID for the initial poll, then we'll switch to the real one if it's not GFE
        NvHTTP http(m_Address, 0, QSslCertificate(), false);

        if (m_Mdns) {
            if (m_MdnsIpv6Address.isNull()) {
                qInfo() << "Processing new PC" << m_Name << "from mDNS with local address" << m_Address.toString();
            }
            else {
                qInfo() << "Processing new PC" << m_Name << "from mDNS with local address" << m_Address.toString()
                        << "and IPv6 address" << m_MdnsIpv6Address.toString();
            }
        }
        else {
            qInfo() << "Processing new PC at" << m_Address.toString() << "from user";
        }

        // Perform initial serverinfo fetch over HTTP since we don't know which cert to use
        QString serverInfo = fetchServerInfo(http);
        if (serverInfo.isEmpty() && !m_MdnsIpv6Address.isNull()) {
            // Retry using the global IPv6 address if the IPv4 or link-local IPv6 address fails
            http.setAddress(m_MdnsIpv6Address);
            serverInfo = fetchServerInfo(http);
        }
        if (serverInfo.isEmpty()) {
            return;
        }

        // Create initial newComputer using HTTP serverinfo with no pinned cert
        NvComputer* newComputer = new NvComputer(http, serverInfo);
        http.setTrueUid(!newComputer->isNvidiaServerSoftware);

        // Check if we have a record of this host UUID to pull the pinned cert
        NvComputer* existingComputer;
        {
            QReadLocker lock(&m_ComputerManager->m_Lock);
            existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            if (existingComputer != nullptr) {
                http.setServerCert(existingComputer->serverCert);
            }
        }

        // Fetch serverinfo again over HTTPS with the pinned cert
        if (existingComputer != nullptr) {
            Q_ASSERT(http.httpsPort() != 0);
            serverInfo = fetchServerInfo(http);
            if (serverInfo.isEmpty()) {
                return;
            }

            // Update the polled computer with the HTTPS information
            NvComputer httpsComputer(http, serverInfo);
            newComputer->update(httpsComputer);
        }

        // Update addresses depending on the context
        if (m_Mdns) {
            // Only update local address if we actually reached the PC via this address.
            // If we reached it via the IPv6 address after the local address failed,
            // don't store the non-working local address.
            if (http.address() == m_Address) {
                newComputer->localAddress = m_Address;
            }

            // Get the WAN IP address using STUN if we're on mDNS over IPv4
            if (QHostAddress(newComputer->localAddress.address()).protocol() == QAbstractSocket::IPv4Protocol) {
                quint32 addr;
                int err = LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &addr);
                if (err == 0) {
                    newComputer->setRemoteAddress(QHostAddress(qFromBigEndian(addr)));
                }
                else {
                    qWarning() << "STUN failed to get WAN address:" << err;
                }
            }

            if (!m_MdnsIpv6Address.isNull()) {
                Q_ASSERT(QHostAddress(m_MdnsIpv6Address.address()).protocol() == QAbstractSocket::IPv6Protocol);
                newComputer->ipv6Address = m_MdnsIpv6Address;
            }

            if (NetLinkFilter::kEnabled) {
                // Retain every address the host advertised, so the poller can
                // still reach a NIC the user allows even when mDNS happened to
                // report a different one first.
                for (const QHostAddress& mdnsAddress : std::as_const(m_MdnsAddresses)) {
                    const NvAddress candidate(mdnsAddress, m_Address.port());
                    if (!newComputer->mdnsAddresses.contains(candidate)) {
                        newComputer->mdnsAddresses.append(candidate);
                    }
                }
            }
        }
        else {
            newComputer->manualAddress = m_Address;
        }

        QHostAddress hostAddress(m_Address.address());
        bool addressIsSiteLocalV4 =
                hostAddress.isInSubnet(QHostAddress("10.0.0.0"), 8) ||
                hostAddress.isInSubnet(QHostAddress("172.16.0.0"), 12) ||
                hostAddress.isInSubnet(QHostAddress("192.168.0.0"), 16);

        {
            // Check if this PC already exists using opportunistic read lock
            m_ComputerManager->m_Lock.lockForRead();
            NvComputer* existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);

            // If it doesn't already exist, convert to a write lock in preparation for updating.
            //
            // NB: ComputerManager's lock protects the host map itself, not the elements inside.
            // Those are protected by their individual locks. Since we only mutate the map itself
            // when the PC doesn't exist, we need the lock in write-mode for that case only.
            if (existingComputer == nullptr) {
                m_ComputerManager->m_Lock.unlock();
                m_ComputerManager->m_Lock.lockForWrite();

                // Since we had to unlock to lock for write, someone could have raced and added
                // this PC before us. We have to check again whether it already exists.
                existingComputer = m_ComputerManager->m_KnownHosts.value(newComputer->uuid);
            }

            if (existingComputer != nullptr) {
                // Fold it into the existing PC
                bool changed = existingComputer->update(*newComputer);
                delete newComputer;

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // For non-mDNS clients, let them know it succeeded
                if (!m_Mdns) {
                    emit computerAddCompleted(true, false);
                }

                // Tell our client if something changed
                if (changed) {
                    qInfo() << existingComputer->name << "is now at" << existingComputer->activeAddress.toString();
                    emit computerStateChanged(existingComputer);
                }
            }
            else {
                // Store this in our active sets
                m_ComputerManager->m_KnownHosts[newComputer->uuid] = newComputer;

                // Start polling if enabled (write lock required)
                m_ComputerManager->startPollingComputer(newComputer);

                // Drop the lock before notifying
                m_ComputerManager->m_Lock.unlock();

                // If this wasn't added via mDNS but it is a RFC 1918 IPv4 address and not a VPN,
                // go ahead and do the STUN request now to populate an external address.
                if (!m_Mdns && addressIsSiteLocalV4 && newComputer->getActiveAddressReachability() != NvComputer::RI_VPN) {
                    quint32 addr;
                    int err = LiFindExternalAddressIP4("stun.moonlight-stream.org", 3478, &addr);
                    if (err == 0) {
                        newComputer->setRemoteAddress(QHostAddress(qFromBigEndian(addr)));
                    }
                    else {
                        qWarning() << "STUN failed to get WAN address:" << err;
                    }
                }

                // For non-mDNS clients, let them know it succeeded
                if (!m_Mdns) {
                    emit computerAddCompleted(true, false);
                }

                // Tell our client about this new PC
                emit computerStateChanged(newComputer);
            }
        }
    }

    ComputerManager* m_ComputerManager;
    QString m_Name;
    NvAddress m_Address;
    NvAddress m_MdnsIpv6Address;
    QVector<QHostAddress> m_MdnsAddresses;
    bool m_Mdns;
    bool m_AboutToQuit;
};

void ComputerManager::addNewHost(NvAddress address, bool mdns, QString name,
                                 NvAddress mdnsIpv6Address, QVector<QHostAddress> mdnsAddresses)
{
    // Punt to a worker thread to avoid stalling the
    // UI while waiting for serverinfo query to complete
    PendingAddTask* addTask = new PendingAddTask(this, name, address, mdnsIpv6Address, mdnsAddresses, mdns);
    QThreadPool::globalInstance()->start(addTask);
}

QString ComputerManager::generatePinString()
{
    return QString::asprintf("%04u", QRandomGenerator::system()->bounded(10000));
}

#include "computermanager.moc"
