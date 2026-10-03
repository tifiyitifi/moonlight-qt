#include "computermodel.h"
#include "backend/netlink.h"

#include <QThreadPool>

#include <utility>

ComputerModel::ComputerModel(QObject* object)
    : QAbstractListModel(object),
      m_CurrentIndex(-1) {}

void ComputerModel::initialize(ComputerManager* computerManager)
{
    m_ComputerManager = computerManager;
    connect(m_ComputerManager, &ComputerManager::computerStateChanged,
            this, &ComputerModel::handleComputerStateChanged);
    connect(m_ComputerManager, &ComputerManager::pairingCompleted,
            this, &ComputerModel::handlePairingCompleted);

    m_Computers = m_ComputerManager->getComputers();
}

QVariant ComputerModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid()) {
        return QVariant();
    }

    if (index.row() < 0 || index.row() >= m_Computers.count()) {
        return QVariant();
    }

    NvComputer* computer = m_Computers[index.row()];

    // This probe takes the computer lock itself, so it has to be resolved
    // before the read lock below is taken.
    QString conflictDescription;
    if (NetLinkFilter::kEnabled && (role == ConnectionConflictRole || role == DetailsRole)) {
        conflictDescription = computer->getConflictingLinkDescription();
    }

    // These roles probe the local routing table, so they are answered before
    // taking the computer lock to keep the lock hold time short.
    if (role == AllowedLinkTypesRole) {
        QReadLocker lock(&computer->lock);
        return NetLinkFilter::kEnabled ? int(computer->allowedLinkTypes)
                                       : int(ComputerModel::NLT_MASK_ALL);
    }

    if (role == ConnectionConflictRole) {
        if (conflictDescription.isEmpty()) {
            return QString();
        }

        return tr("Windows will send the traffic to this PC through %1 even though the "
                  "connection types selected here do not allow it. Several of your network "
                  "adapters are in the same IP subnet, so the destination address cannot "
                  "select one and the interface metric decides instead. Lower the Interface "
                  "Metric of the adapter you want to use, or put the adapters on separate "
                  "subnets.").arg(conflictDescription);
    }

    if (role == UnenforceableLinkFilterRole) {
        // A different failure from the one above: there the mask is honored but
        // the OS overrides it, here there is no address to honor it with. The
        // name is reported back so the user can see which entry it came from.
        //
        // The wording does not claim the name failed to resolve, because it
        // need not have: it covers the window where the name has not resolved
        // to anything we can judge yet (nothing is known about the host's
        // addresses, so nothing was excluded), and in that window
        // allowedAddresses() keeps the name, the poller still probes it, and a
        // host that answers is streamed over whatever the name reaches. Every
        // state in which a name is NOT in the candidate list is a different fact
        // with the opposite consequence, and each is its own role below:
        // MaskExcludedLinkFilterRole when the mask turned down every literal, and
        // NameSuppressedLinkFilterRole when the mask approved one that is not
        // answering. All three are worth saying separately - they send the user
        // to three different places.
        const QString unclassifiable = computer->getUnclassifiableAddress();
        if (unclassifiable.isEmpty()) {
            return QString();
        }

        return tr("This PC is addressed by the name \"%1\". Moonlight can only apply "
                  "the connection types selected here to an IP address, and right now it "
                  "has none for this PC to judge, so the selection is not in effect: "
                  "the stream will use whichever network adapter the name reaches and "
                  "Windows routes over. Add the PC by its IP address, or let Moonlight "
                  "discover it over mDNS, to get the selection back.")
               .arg(unclassifiable);
    }

    if (role == MaskExcludedLinkFilterRole) {
        // The mirror image of the role above, and the reason it cannot share its
        // wording: the mask IS in effect here, it is simply excluding every
        // address this host can be reached on. allowedAddresses() dropped them
        // and the hostname with them, so the poller is probing nothing and the PC
        // is offline - there is no stream to describe. Naming the types is the
        // whole of the remedy, so the message is built around them, and it
        // stays phrased for one type or several. Nothing in the mask can be
        // allowed at this point, since a single allowed type would have made
        // the host reachable, so every type named here is one the user
        // deselected.
        const quint32 excludedTypes = computer->getMaskExcludedLinkTypes();
        if (excludedTypes == 0) {
            return QString();
        }

        return tr("Moonlight is not using this PC: every address it can be reached on "
                  "needs a connection type that is not selected here (%1), so the PC is "
                  "shown as offline. Select one of them for this PC to reach it.")
               .arg(linkTypeMaskDescription(excludedTypes));
    }

    if (role == NameSuppressedLinkFilterRole) {
        // The third state, and the only one that is a side effect of enforcing
        // the mask rather than a statement about it: the mask approved an
        // address, that address is not answering, and the name which would still
        // work cannot be used because the mask cannot be applied to it. It needs
        // its own wording because the remedy is not in the connection type
        // selection at all - nothing there is wrong - it is in the address being
        // out of date, so naming types here would send the user to fix
        // something that is already correct.
        const QString suppressedName = computer->getNameSuppressedAddress();
        if (suppressedName.isEmpty()) {
            return QString();
        }

        return tr("Moonlight is not using the name \"%1\" to reach this PC, because the "
                  "connection types selected here can only be applied to an IP address. It "
                  "is using the address that name resolved to, and that address is not "
                  "answering. If this PC is switched on and reachable, it may have a "
                  "different address now - Moonlight looks the name up again on its own, "
                  "or you can add the PC by its IP address.")
               .arg(suppressedName);
    }

    if (role == ActiveRouteDescriptionRole) {
        NvAddress active;
        {
            QReadLocker lock(&computer->lock);
            active = computer->activeAddress;
        }

        if (!NetLinkFilter::kEnabled || active.isNull()) {
            return QString();
        }

        const QVector<NetRoute> routes = netRoutesTo(QHostAddress(active.address()), active.port());
        if (routes.isEmpty()) {
            return QString();
        }

        // Report every route to the active address so the user can see which
        // adapters are in play, and which one Windows would pick.
        QStringList descriptions;
        for (const NetRoute& route : std::as_const(routes)) {
            QString entry = netLinkTypeName(route.bit()) + QString(" \u2014 ") + route.nicDescription +
                            QString(" (") + route.localAddress.toString();
            if (route.prefixLength >= 0) {
                entry += "/" + QString::number(route.prefixLength);
            }
            entry += QString(")");
            if (route.isPreferred) {
                entry += tr(", in use by Windows");
            }
            descriptions.append(entry);
        }

        return descriptions.join(QLatin1Char('\n'));
    }

    QReadLocker lock(&computer->lock);

    switch (role) {
    case NameRole:
        return computer->name;
    case OnlineRole:
        return computer->state == NvComputer::CS_ONLINE;
    case PairedRole:
        return computer->pairState == NvComputer::PS_PAIRED;
    case BusyRole:
        return computer->currentGameId != 0;
    case WakeableRole:
        return !computer->macAddress.isEmpty();
    case StatusUnknownRole:
        return computer->state == NvComputer::CS_UNKNOWN;
    case ServerSupportedRole:
        return computer->isSupportedServerVersion;
    case DetailsRole: {
        QString state, pairState;

        switch (computer->state) {
        case NvComputer::CS_ONLINE:
            state = tr("Online");
            break;
        case NvComputer::CS_OFFLINE:
            state = tr("Offline");
            break;
        default:
            state = tr("Unknown");
            break;
        }

        switch (computer->pairState) {
        case NvComputer::PS_PAIRED:
            pairState = tr("Paired");
            break;
        case NvComputer::PS_NOT_PAIRED:
            pairState = tr("Unpaired");
            break;
        default:
            pairState = tr("Unknown");
            break;
        }

        return tr("Name: %1").arg(computer->name) + '\n' +
               tr("Status: %1").arg(state) + '\n' +
               tr("Active Address: %1").arg(computer->activeAddress.toString()) + '\n' +
               tr("UUID: %1").arg(computer->uuid) + '\n' +
               tr("Local Address: %1").arg(computer->localAddress.toString()) + '\n' +
               tr("Remote Address: %1").arg(computer->remoteAddress.toString()) + '\n' +
               tr("IPv6 Address: %1").arg(computer->ipv6Address.toString()) + '\n' +
               tr("Manual Address: %1").arg(computer->manualAddress.toString()) + '\n' +
               tr("MAC Address: %1").arg(computer->macAddress.isEmpty() ? tr("Unknown") : QString(computer->macAddress.toHex(':'))) + '\n' +
               (NetLinkFilter::kEnabled ? tr("Allowed Connections: %1").arg(describeAllowedLinkTypes(computer->allowedLinkTypes)) + '\n' : QString()) +
               (NetLinkFilter::kEnabled ? tr("Current Route: %1").arg(conflictDescription.isEmpty() ?
                    (computer->activeAddress.isNull() ? tr("Unknown") : computer->activeAddress.toString()) :
                    tr("%1 (not allowed)").arg(conflictDescription)) + '\n' : QString()) +
               tr("Pair State: %1").arg(pairState) + '\n' +
               tr("Running Game ID: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->currentGameId) : tr("Unknown")) + '\n' +
               tr("HTTPS Port: %1").arg(computer->state == NvComputer::CS_ONLINE ? QString::number(computer->activeHttpsPort) : tr("Unknown"));
    }
    default:
        return QVariant();
    }
}

int ComputerModel::rowCount(const QModelIndex& parent) const
{
    // We should not return a count for valid index values,
    // only the parent (which will not have a "valid" index).
    if (parent.isValid()) {
        return 0;
    }

    return m_Computers.count();
}

QHash<int, QByteArray> ComputerModel::roleNames() const
{
    QHash<int, QByteArray> names;

    names[NameRole] = "name";
    names[OnlineRole] = "online";
    names[PairedRole] = "paired";
    names[BusyRole] = "busy";
    names[WakeableRole] = "wakeable";
    names[StatusUnknownRole] = "statusUnknown";
    names[ServerSupportedRole] = "serverSupported";
    names[DetailsRole] = "details";
    names[AllowedLinkTypesRole] = "allowedLinkTypes";
    names[ActiveRouteDescriptionRole] = "activeRoute";
    names[ConnectionConflictRole] = "connectionConflict";
    names[UnenforceableLinkFilterRole] = "unenforceableLinkFilter";
    names[MaskExcludedLinkFilterRole] = "maskExcludedLinkFilter";
    names[NameSuppressedLinkFilterRole] = "nameSuppressedLinkFilter";

    return names;
}

QString ComputerModel::netLinkTypeName(int netLinkType) const
{
    switch (netLinkType) {
    case NLT_MASK_WIRED:
        return tr("Ethernet (Wired)");
    case NLT_MASK_WIRELESS:
        return tr("Wi-Fi (Wireless)");
    case NLT_MASK_VIRTUAL:
        return tr("VPN / Virtual Adapter");
    case NLT_MASK_OTHER:
        return tr("Other / Unknown");
    default:
        return tr("Other / Unknown");
    }
}

QString ComputerModel::describeAllowedLinkTypes(quint32 allowedLinkTypes) const
{
    if (!NetLinkFilter::kEnabled || allowedLinkTypes == NLT_MASK_ALL) {
        return tr("All");
    }

    return linkTypeMaskDescription(allowedLinkTypes);
}

QString ComputerModel::linkTypeMaskDescription(quint32 linkTypes) const
{
    QStringList names;
    for (int bit = NLT_MASK_WIRED; bit <= NLT_MASK_OTHER; bit <<= 1) {
        if (linkTypes & quint32(bit)) {
            names.append(netLinkTypeName(bit));
        }
    }

    return names.join(QStringLiteral(", "));
}

QVariant ComputerModel::currentValue(int role) const
{
    if (m_CurrentIndex < 0 || m_CurrentIndex >= m_Computers.count()) {
        return QVariant();
    }

    return data(createIndex(m_CurrentIndex, 0), role);
}

void ComputerModel::reindexCurrentComputer()
{
    m_CurrentIndex = -1;
    if (m_CurrentUuid.isEmpty()) {
        return;
    }

    for (int i = 0; i < m_Computers.count(); i++) {
        if (m_Computers[i]->uuid == m_CurrentUuid) {
            m_CurrentIndex = i;
            break;
        }
    }
}

int ComputerModel::currentIndex() const
{
    return m_CurrentIndex;
}

void ComputerModel::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_Computers.count()) {
        index = -1;
    }

    // Track the selected host by identity, since indices shift when the list
    // is refreshed (a host is added or removed).
    m_CurrentUuid = index >= 0 ? m_Computers[index]->uuid : QString();

    if (m_CurrentIndex == index) {
        return;
    }

    m_CurrentIndex = index;
    emit currentChanged();
}

bool ComputerModel::setCurrentIndexByUuid(const QString& uuid)
{
    int index = -1;
    if (!uuid.isEmpty()) {
        for (int i = 0; i < m_Computers.count(); i++) {
            if (m_Computers[i]->uuid == uuid) {
                index = i;
                break;
            }
        }
    }

    m_CurrentUuid = index >= 0 ? m_Computers[index]->uuid : uuid;

    if (m_CurrentIndex == index) {
        return index >= 0;
    }

    m_CurrentIndex = index;
    emit currentChanged();
    return index >= 0;
}

QString ComputerModel::getComputerUuid(int computerIndex) const
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return QString();
    }

    QReadLocker lock(&m_Computers[computerIndex]->lock);
    return m_Computers[computerIndex]->uuid;
}

QString ComputerModel::currentName() const
{
    return currentValue(NameRole).toString();
}

int ComputerModel::currentAllowedLinkTypes() const
{
    return currentValue(AllowedLinkTypesRole).toInt();
}

QString ComputerModel::currentActiveRoute() const
{
    return currentValue(ActiveRouteDescriptionRole).toString();
}

QString ComputerModel::currentConnectionConflict() const
{
    return currentValue(ConnectionConflictRole).toString();
}

QString ComputerModel::currentUnenforceableLinkFilter() const
{
    return currentValue(UnenforceableLinkFilterRole).toString();
}

QString ComputerModel::currentMaskExcludedLinkFilter() const
{
    return currentValue(MaskExcludedLinkFilterRole).toString();
}

QString ComputerModel::currentNameSuppressedLinkFilter() const
{
    return currentValue(NameSuppressedLinkFilterRole).toString();
}

bool ComputerModel::netLinkFilterEnabled() const
{
    return NetLinkFilter::kEnabled;
}

void ComputerModel::setConnectionOptionsForComputer(int computerIndex, int allowedLinkTypes)
{
    if (computerIndex < 0 || computerIndex >= m_Computers.count()) {
        return;
    }

    m_ComputerManager->setHostConnectionOptions(m_Computers[computerIndex],
                                                quint32(allowedLinkTypes));
}

void ComputerModel::setConnectionOptionsForCurrentComputer(int allowedLinkTypes)
{
    if (m_CurrentIndex < 0 || m_CurrentIndex >= m_Computers.count()) {
        return;
    }

    m_ComputerManager->setHostConnectionOptions(m_Computers[m_CurrentIndex],
                                                quint32(allowedLinkTypes));
}

Session* ComputerModel::createSessionForCurrentGame(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    NvComputer* computer = m_Computers[computerIndex];

    // We must currently be streaming a game to use this function
    Q_ASSERT(computer->currentGameId != 0);

    for (NvApp& app : computer->appList) {
        if (app.id == computer->currentGameId) {
            return new Session(computer, app);
        }
    }

    // We have a current running app but it's not in our app list
    Q_ASSERT(false);
    return nullptr;
}

void ComputerModel::deleteComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    beginRemoveRows(QModelIndex(), computerIndex, computerIndex);

    // m_Computer[computerIndex] will be deleted by this call
    m_ComputerManager->deleteHost(m_Computers[computerIndex]);

    // Remove the now invalid item
    m_Computers.removeAt(computerIndex);

    endRemoveRows();

    // Indices are positional, so re-resolve the selected host by identity.
    // Without this, currentValue() (and
    // setConnectionOptionsForCurrentComputer()) would silently point at a
    // different PC after the row above the selection disappears, and would
    // read a position that no longer exists when the last row goes away.
    reindexCurrentComputer();
    emit currentChanged();
}

class DeferredWakeHostTask : public QRunnable
{
public:
    DeferredWakeHostTask(NvComputer* computer)
        : m_Computer(computer) {}

    void run()
    {
        m_Computer->wake();
    }

private:
    NvComputer* m_Computer;
};

void ComputerModel::wakeComputer(int computerIndex)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    DeferredWakeHostTask* wakeTask = new DeferredWakeHostTask(m_Computers[computerIndex]);
    QThreadPool::globalInstance()->start(wakeTask);
}

void ComputerModel::renameComputer(int computerIndex, QString name)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->renameHost(m_Computers[computerIndex], name);
}

QString ComputerModel::generatePinString()
{
    return m_ComputerManager->generatePinString();
}

class DeferredTestConnectionTask : public QObject, public QRunnable
{
    Q_OBJECT
public:
    void run()
    {
        unsigned int portTestResult = LiTestClientConnectivity("qt.conntest.moonlight-stream.org", 443, ML_PORT_FLAG_ALL);
        if (portTestResult == ML_TEST_RESULT_INCONCLUSIVE) {
            emit connectionTestCompleted(-1, QString());
        }
        else {
            char blockedPorts[512];
            LiStringifyPortFlags(portTestResult, "\n", blockedPorts, sizeof(blockedPorts));
            emit connectionTestCompleted(portTestResult, QString(blockedPorts));
        }
    }

signals:
    void connectionTestCompleted(int result, QString blockedPorts);
};

void ComputerModel::testConnectionForComputer(int)
{
    DeferredTestConnectionTask* testConnectionTask = new DeferredTestConnectionTask();
    QObject::connect(testConnectionTask, &DeferredTestConnectionTask::connectionTestCompleted,
                     this, &ComputerModel::connectionTestCompleted);
    QThreadPool::globalInstance()->start(testConnectionTask);
}

void ComputerModel::pairComputer(int computerIndex, QString pin)
{
    Q_ASSERT(computerIndex < m_Computers.count());

    m_ComputerManager->pairHost(m_Computers[computerIndex], pin);
}

void ComputerModel::handlePairingCompleted(NvComputer*, QString error)
{
    emit pairingCompleted(error.isEmpty() ? QVariant() : error);
}

void ComputerModel::handleComputerStateChanged(NvComputer* computer)
{
    QVector<NvComputer*> newComputerList = m_ComputerManager->getComputers();

    // Reset the model if the structural layout of the list has changed
    if (m_Computers != newComputerList) {
        beginResetModel();
        m_Computers = newComputerList;

        // Indices are positional, so re-resolve the selected host by identity.
        // Without this, currentValue() (and setConnectionOptionsForCurrentComputer())
        // would silently point at a different PC after the list is reordered.
        reindexCurrentComputer();

        endResetModel();
    }
    else {
        // Let the view know that this specific computer changed
        int index = m_Computers.indexOf(computer);
        emit dataChanged(createIndex(index, 0), createIndex(index, 0));
    }

    // The current* properties read through data(), so they need their own
    // notification for pages that are not backed by a delegate.
    emit currentChanged();
}

#include "computermodel.moc"
