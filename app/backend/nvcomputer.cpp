#include "nvcomputer.h"
#include "nvapp.h"
#include "netlink.h"
#include "settings/compatfetcher.h"

#include <QUdpSocket>
#include <QHostInfo>
#include <QNetworkInterface>
#include <QNetworkProxy>

#include <utility>

#define SER_NAME "hostname"
#define SER_UUID "uuid"
#define SER_MAC "mac"
#define SER_LOCALADDR "localaddress"
#define SER_LOCALPORT "localport"
#define SER_REMOTEADDR "remoteaddress"
#define SER_REMOTEPORT "remoteport"
#define SER_MANUALADDR "manualaddress"
#define SER_MANUALPORT "manualport"
#define SER_IPV6ADDR "ipv6address"
#define SER_IPV6PORT "ipv6port"
#define SER_APPLIST "apps"
#define SER_SRVCERT "srvcert"
#define SER_CUSTOMNAME "customname"
#define SER_NVIDIASOFTWARE "nvidiasw"
#define SER_MDNSADDRS "mdnsaddresses"
#define SER_RESOLVEDADDRS "resolvedaddresses"
#define SER_ALLOWEDLINKTYPES "allowedlinktypes"

NvComputer::NvComputer(QSettings& settings)
{
    this->name = settings.value(SER_NAME).toString();
    this->uuid = settings.value(SER_UUID).toString();
    this->hasCustomName = settings.value(SER_CUSTOMNAME).toBool();
    this->macAddress = settings.value(SER_MAC).toByteArray();
    this->localAddress = NvAddress(settings.value(SER_LOCALADDR).toString(),
                                   settings.value(SER_LOCALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->remoteAddress = NvAddress(settings.value(SER_REMOTEADDR).toString(),
                                    settings.value(SER_REMOTEPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->ipv6Address = NvAddress(settings.value(SER_IPV6ADDR).toString(),
                                  settings.value(SER_IPV6PORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->manualAddress = NvAddress(settings.value(SER_MANUALADDR).toString(),
                                    settings.value(SER_MANUALPORT, QVariant(DEFAULT_HTTP_PORT)).toUInt());
    this->serverCert = QSslCertificate(settings.value(SER_SRVCERT).toByteArray());
    this->isNvidiaServerSoftware = settings.value(SER_NVIDIASOFTWARE).toBool();

    // Absent key means a host saved before this feature existed, which must
    // keep behaving exactly like it did before, so the default is NLT_ALL.
    // Sanitize hand-edited or corrupted values: zero or unknown bits would
    // otherwise leave the host permanently falling back to the unfiltered list.
    this->allowedLinkTypes = settings.value(SER_ALLOWEDLINKTYPES, QVariant(NLT_ALL)).toUInt();
    if (this->allowedLinkTypes == 0 || (this->allowedLinkTypes & ~quint32(NLT_ALL)) != 0) {
        this->allowedLinkTypes = NLT_ALL;
    }

    int mdnsAddrCount = settings.beginReadArray(SER_MDNSADDRS);
    this->mdnsAddresses.reserve(mdnsAddrCount);
    for (int i = 0; i < mdnsAddrCount; i++) {
        settings.setArrayIndex(i);
        this->mdnsAddresses.append(NvAddress(settings.value("address").toString(),
                                             settings.value("port", QVariant(DEFAULT_HTTP_PORT)).toUInt()));
    }
    settings.endArray();

    int resolvedAddrCount = settings.beginReadArray(SER_RESOLVEDADDRS);
    this->resolvedAddresses.reserve(resolvedAddrCount);
    for (int i = 0; i < resolvedAddrCount; i++) {
        settings.setArrayIndex(i);
        this->resolvedAddresses.append(NvAddress(settings.value("address").toString(),
                                                 settings.value("port", QVariant(DEFAULT_HTTP_PORT)).toUInt()));
    }
    settings.endArray();

    int appCount = settings.beginReadArray(SER_APPLIST);
    this->appList.reserve(appCount);
    for (int i = 0; i < appCount; i++) {
        settings.setArrayIndex(i);

        NvApp app(settings);
        this->appList.append(app);
    }
    settings.endArray();
    sortAppList();

    this->currentGameId = 0;
    this->pairState = PS_UNKNOWN;
    this->state = CS_UNKNOWN;
    this->gfeVersion = nullptr;
    this->appVersion = nullptr;
    this->maxLumaPixelsHEVC = 0;
    this->serverCodecModeSupport = 0;
    this->pendingQuit = false;
    this->gpuModel = nullptr;
    this->isSupportedServerVersion = true;
    this->externalPort = this->remoteAddress.port();
    this->activeHttpsPort = 0;
}

void NvComputer::setRemoteAddress(QHostAddress address)
{
    QWriteLocker lock(&this->lock);

    Q_ASSERT(this->externalPort != 0);

    this->remoteAddress = NvAddress(address, this->externalPort);
}

void NvComputer::serialize(QSettings& settings, bool serializeApps) const
{
    QReadLocker lock(&this->lock);

    settings.setValue(SER_NAME, name);
    settings.setValue(SER_CUSTOMNAME, hasCustomName);
    settings.setValue(SER_UUID, uuid);
    settings.setValue(SER_MAC, macAddress);
    settings.setValue(SER_LOCALADDR, localAddress.address());
    settings.setValue(SER_LOCALPORT, localAddress.port());
    settings.setValue(SER_REMOTEADDR, remoteAddress.address());
    settings.setValue(SER_REMOTEPORT, remoteAddress.port());
    settings.setValue(SER_IPV6ADDR, ipv6Address.address());
    settings.setValue(SER_IPV6PORT, ipv6Address.port());
    settings.setValue(SER_MANUALADDR, manualAddress.address());
    settings.setValue(SER_MANUALPORT, manualAddress.port());
    settings.setValue(SER_SRVCERT, serverCert.toPem());
    settings.setValue(SER_NVIDIASOFTWARE, isNvidiaServerSoftware);
    // Deliberately not written while the filter is compiled out: leaving the
    // stored mask untouched is what makes the flag behave like a kill switch.
    // A choice the user made while the feature was live survives on disk and
    // takes effect again if the flag is flipped back, where writing NLT_ALL
    // here would silently discard it.
    if (NetLinkFilter::kEnabled) {
        settings.setValue(SER_ALLOWEDLINKTYPES, allowedLinkTypes);
    }

    // Avoid deleting an existing mDNS address list if we couldn't get one
    if (NetLinkFilter::kEnabled && !mdnsAddresses.isEmpty()) {
        settings.remove(SER_MDNSADDRS);
        settings.beginWriteArray(SER_MDNSADDRS);
        for (int i = 0; i < mdnsAddresses.count(); i++) {
            settings.setArrayIndex(i);
            settings.setValue("address", mdnsAddresses.at(i).address());
            settings.setValue("port", mdnsAddresses.at(i).port());
        }
        settings.endArray();
    }

    // Unlike the mDNS list, an empty one here is meaningful rather than
    // missing: ComputerManager clears it when the mask goes back to NLT_ALL,
    // so a host the user has no preference for keeps the exact address list it
    // had before this feature existed. Leaving stale entries on disk would
    // resurrect them on the next launch, so the key is dropped either way.
    // Skipped entirely while the filter is compiled out, for the same reason
    // SER_ALLOWEDLINKTYPES is: the stored choice has to survive the flag.
    if (NetLinkFilter::kEnabled) {
        settings.remove(SER_RESOLVEDADDRS);
        if (!resolvedAddresses.isEmpty()) {
            settings.beginWriteArray(SER_RESOLVEDADDRS);
            for (int i = 0; i < resolvedAddresses.count(); i++) {
                settings.setArrayIndex(i);
                settings.setValue("address", resolvedAddresses.at(i).address());
                settings.setValue("port", resolvedAddresses.at(i).port());
            }
            settings.endArray();
        }
    }

    // Avoid deleting an existing applist if we couldn't get one
    if (!appList.isEmpty() && serializeApps) {
        settings.remove(SER_APPLIST);
        settings.beginWriteArray(SER_APPLIST);
        for (int i = 0; i < appList.count(); i++) {
            settings.setArrayIndex(i);
            appList.at(i).serialize(settings);
        }
        settings.endArray();
    }
}

bool NvComputer::isEqualSerialized(const NvComputer &that) const
{
    return this->name == that.name &&
           this->hasCustomName == that.hasCustomName &&
           this->uuid == that.uuid &&
           this->macAddress == that.macAddress &&
           this->localAddress == that.localAddress &&
           this->remoteAddress == that.remoteAddress &&
           this->ipv6Address == that.ipv6Address &&
           this->manualAddress == that.manualAddress &&
           this->serverCert == that.serverCert &&
           this->isNvidiaServerSoftware == that.isNvidiaServerSoftware &&
           this->mdnsAddresses == that.mdnsAddresses &&
           this->resolvedAddresses == that.resolvedAddresses &&
           this->allowedLinkTypes == that.allowedLinkTypes &&
           this->appList == that.appList;
}

void NvComputer::sortAppList()
{
    std::stable_sort(appList.begin(), appList.end(), [](const NvApp& app1, const NvApp& app2) {
       return app1.name.toLower() < app2.name.toLower();
    });
}

NvComputer::NvComputer(NvHTTP& http, QString serverInfo)
{
    this->serverCert = http.serverCert();

    this->hasCustomName = false;
    this->name = NvHTTP::getXmlString(serverInfo, "hostname");
    if (this->name.isEmpty()) {
        this->name = "UNKNOWN";
    }

    // A freshly polled host has no user preference yet. update() must never
    // propagate this over the stored value.
    this->allowedLinkTypes = NLT_ALL;

    this->uuid = NvHTTP::getXmlString(serverInfo, "uniqueid");
    QString newMacString = NvHTTP::getXmlString(serverInfo, "mac");
    if (newMacString != "00:00:00:00:00:00") {
        QStringList macOctets = newMacString.split(':');
        for (const QString& macOctet : std::as_const(macOctets)) {
            this->macAddress.append((char) macOctet.toInt(nullptr, 16));
        }
    }

    QString codecSupport = NvHTTP::getXmlString(serverInfo, "ServerCodecModeSupport");
    if (!codecSupport.isEmpty()) {
        this->serverCodecModeSupport = codecSupport.toInt();
    }
    else {
        // Assume H.264 is always supported
        this->serverCodecModeSupport = SCM_H264;
    }

    QString maxLumaPixelsHEVC = NvHTTP::getXmlString(serverInfo, "MaxLumaPixelsHEVC");
    if (!maxLumaPixelsHEVC.isEmpty()) {
        this->maxLumaPixelsHEVC = maxLumaPixelsHEVC.toInt();
    }
    else {
        this->maxLumaPixelsHEVC = 0;
    }

    this->displayModes = NvHTTP::getDisplayModeList(serverInfo);
    std::stable_sort(this->displayModes.begin(), this->displayModes.end(),
                     [](const NvDisplayMode& mode1, const NvDisplayMode& mode2) {
        return (uint64_t)mode1.width * mode1.height * mode1.refreshRate <
                (uint64_t)mode2.width * mode2.height * mode2.refreshRate;
    });

    // We can get an IPv4 loopback address if we're using the GS IPv6 Forwarder
    this->localAddress = NvAddress(NvHTTP::getXmlString(serverInfo, "LocalIP"), http.httpPort());
    if (this->localAddress.address().startsWith("127.")) {
        this->localAddress = NvAddress();
    }

    QString httpsPort = NvHTTP::getXmlString(serverInfo, "HttpsPort");
    if (httpsPort.isEmpty() || (this->activeHttpsPort = httpsPort.toUShort()) == 0) {
        this->activeHttpsPort = DEFAULT_HTTPS_PORT;
    }

    // This is an extension which is not present in GFE. It is present for Sunshine to be able
    // to support dynamic HTTP WAN ports without requiring the user to manually enter the port.
    QString remotePortStr = NvHTTP::getXmlString(serverInfo, "ExternalPort");
    if (remotePortStr.isEmpty() || (this->externalPort = remotePortStr.toUShort()) == 0) {
        this->externalPort = http.httpPort();
    }

    QString remoteAddress = NvHTTP::getXmlString(serverInfo, "ExternalIP");
    if (!remoteAddress.isEmpty()) {
        this->remoteAddress = NvAddress(remoteAddress, this->externalPort);
    }
    else {
        this->remoteAddress = NvAddress();
    }

    // Real Nvidia host software (GeForce Experience and RTX Experience) both use the 'Mjolnir'
    // codename in the state field and no version of Sunshine does. We can use this to bypass
    // some assumptions about Nvidia hardware that don't apply to Sunshine hosts.
    this->isNvidiaServerSoftware = NvHTTP::getXmlString(serverInfo, "state").contains("MJOLNIR");

    this->pairState = NvHTTP::getXmlString(serverInfo, "PairStatus") == "1" ?
                PS_PAIRED : PS_NOT_PAIRED;
    this->currentGameId = NvHTTP::getCurrentGame(serverInfo);
    this->appVersion = NvHTTP::getXmlString(serverInfo, "appversion");
    this->gfeVersion = NvHTTP::getXmlString(serverInfo, "GfeVersion");
    this->gpuModel = NvHTTP::getXmlString(serverInfo, "gputype");
    this->activeAddress = http.address();
    this->state = NvComputer::CS_ONLINE;
    this->pendingQuit = false;
    this->isSupportedServerVersion = CompatFetcher::isGfeVersionSupported(this->gfeVersion);
}

bool NvComputer::wake() const
{
    QByteArray wolPayload;

    {
        QReadLocker readLocker(&lock);

        if (state == NvComputer::CS_ONLINE) {
            qWarning() << name << "is already online";
            return true;
        }

        if (macAddress.isEmpty()) {
            qWarning() << name << "has no MAC address stored";
            return false;
        }

        // Create the WoL payload
        wolPayload.append(QByteArray::fromHex("FFFFFFFFFFFF"));
        for (int i = 0; i < 16; i++) {
            wolPayload.append(macAddress);
        }
        Q_ASSERT(wolPayload.size() == 102);
    }

    // Ports used as-is
    const quint16 STATIC_WOL_PORTS[] = {
        9, // Standard WOL port (privileged port)
        47009, // Port opened by Moonlight Internet Hosting Tool for WoL (non-privileged port)
    };

    // Ports offset by the HTTP base port for hosts using alternate ports
    const quint16 DYNAMIC_WOL_PORTS[] = {
        47998, 47999, 48000, 48002, 48010, // Ports opened by GFE
    };

    // Add the addresses that we know this host to be
    // and broadcast addresses for this link just in
    // case the host has timed out in ARP entries.
    QMap<QString, quint16> addressMap;
    QSet<quint16> basePortSet;
    const auto uniqueHostAddresses = uniqueAddresses();
    for (const NvAddress& addr : uniqueHostAddresses) {
        addressMap.insert(addr.address(), addr.port());
        basePortSet.insert(addr.port());
    }
    addressMap.insert("255.255.255.255", 0);

    // Try to broadcast on all available NICs
    const auto allInterfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface& nic : allInterfaces) {
        // Ensure the interface is up and skip the loopback adapter
        if ((nic.flags() & QNetworkInterface::IsUp) == 0 ||
                (nic.flags() & QNetworkInterface::IsLoopBack) != 0) {
            continue;
        }

        QHostAddress allNodesMulticast("FF02::1");
        const auto allInterfaceAddresses = nic.addressEntries();
        for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
            // Store the scope ID for this NIC if IPv6 is enabled
            if (!addr.ip().scopeId().isEmpty()) {
                allNodesMulticast.setScopeId(addr.ip().scopeId());
            }

            // Skip IPv6 which doesn't support broadcast
            if (!addr.broadcast().isNull()) {
                addressMap.insert(addr.broadcast().toString(), 0);
            }
        }

        if (!allNodesMulticast.scopeId().isEmpty()) {
            addressMap.insert(allNodesMulticast.toString(), 0);
        }
    }

    // Try all unique address strings or host names
    bool success = false;
    for (auto i = addressMap.constBegin(); i != addressMap.constEnd(); i++) {
        QHostAddress literalAddress;
        QList<QHostAddress> addressList;

        // If this is an IPv4/IPv6 literal, don't use QHostInfo::fromName() because that will
        // try to perform a reverse DNS lookup that leads to delays sending WoL packets.
        if (literalAddress.setAddress(i.key())) {
            addressList.append(literalAddress);
        }
        else {
            QHostInfo hostInfo = QHostInfo::fromName(i.key());
            if (hostInfo.error() != QHostInfo::NoError) {
                qWarning() << "Error resolving" << i.key() << ":" << hostInfo.errorString();
                continue;
            }

            addressList.append(hostInfo.addresses());
        }

        // Try all IP addresses that this string resolves to
        for (QHostAddress& address : addressList) {
            QUdpSocket sock;

            // Send to all static ports
            for (quint16 port : STATIC_WOL_PORTS) {
                if (sock.writeDatagram(wolPayload, address, port)) {
                    qInfo().nospace().noquote() << "Sent WoL packet to " << name << " via " << address.toString() << ":" << port;
                    success = true;
                }
                else {
                    qWarning() << "Send failed:" << sock.error();
                }
            }

            QList<quint16> basePorts;
            if (i.value() != 0) {
                // If we have a known base port for this address, use only that port
                basePorts.append(i.value());
            }
            else {
                // If this is a broadcast address without a known HTTP port, try all of them
                basePorts.append(basePortSet.values());
            }

            // Send to all dynamic ports using the HTTP port offset(s) for this address
            for (quint16 basePort : basePorts) {
                for (quint16 port : DYNAMIC_WOL_PORTS) {
                    port = (port - 47989) + basePort;

                    if (sock.writeDatagram(wolPayload, address, port)) {
                        qInfo().nospace().noquote() << "Sent WoL packet to " << name << " via " << address.toString() << ":" << port;
                        success = true;
                    }
                    else {
                        qWarning() << "Send failed:" << sock.error();
                    }
                }
            }
        }
    }

    return success;
}

NvComputer::ReachabilityType NvComputer::getActiveAddressReachability() const
{
    NvAddress copyOfActiveAddress;

    {
        QReadLocker readLocker(&lock);

        if (activeAddress.isNull()) {
            return ReachabilityType::RI_UNKNOWN;
        }

        // Grab a copy of the active address to avoid having to hold
        // the computer lock while doing socket operations
        copyOfActiveAddress = activeAddress;
    }

    QTcpSocket s;
    s.setProxy(QNetworkProxy::NoProxy);
    s.connectToHost(copyOfActiveAddress.address(), copyOfActiveAddress.port());
    if (s.waitForConnected(3000)) {
        Q_ASSERT(!s.localAddress().isNull());
        Q_ASSERT(!s.peerAddress().isNull());

        const auto allInterfaces = QNetworkInterface::allInterfaces();
        for (const QNetworkInterface& nic : allInterfaces) {
            // Ensure the interface is up
            if ((nic.flags() & QNetworkInterface::IsUp) == 0) {
                continue;
            }

            const auto allInterfaceAddresses = nic.addressEntries();
            for (const QNetworkAddressEntry& addr : allInterfaceAddresses) {
                if (addr.ip() == s.localAddress()) {
                    qInfo() << "Found matching interface:" << nic.humanReadableName() << nic.hardwareAddress() << nic.flags();

#if QT_VERSION >= QT_VERSION_CHECK(5, 11, 0)
                    qInfo() << "Interface Type:" << nic.type();
                    qInfo() << "Interface MTU:" << nic.maximumTransmissionUnit();

                    if (nic.type() == QNetworkInterface::Virtual ||
                            nic.type() == QNetworkInterface::Ppp) {
                        // Treat PPP and virtual interfaces as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.maximumTransmissionUnit() != 0 && nic.maximumTransmissionUnit() < 1500) {
                        // Treat MTUs under 1500 as likely VPNs
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.flags() & QNetworkInterface::IsPointToPoint) {
                        // Treat point-to-point links as likely VPNs.
                        // This check detects OpenVPN on Unix-like OSes.
                        return ReachabilityType::RI_VPN;
                    }

#ifdef Q_OS_WINDOWS
                    if (nic.name().startsWith("iftype53_") || nic.name().startsWith("iftype131_")) {
                        // Match by NDIS interface type. These values are Microsoft's recommended values for VPN connections:
                        // https://learn.microsoft.com/en-US/troubleshoot/windows-client/networking/windows-connection-manager-disconnects-wlan#more-information
                        //
                        // The following VPNs use IF_TYPE_PROP_VIRTUAL under Windows:
                        //  - WireguardNT VPNs
                        //  - All WinTun-based VPNs (such as Slack Nebula)
                        //  - OpenVPN with tap-windows6
                        return ReachabilityType::RI_VPN;
                    }
#endif

                    if (nic.hardwareAddress().startsWith("00:FF", Qt::CaseInsensitive)) {
                        // OpenVPN TAP interfaces have a MAC address starting with 00:FF on Windows
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().startsWith("ZeroTier")) {
                        // ZeroTier interfaces always start with "ZeroTier"
                        return ReachabilityType::RI_VPN;
                    }

                    if (nic.humanReadableName().contains("VPN")) {
                        // This one is just a final VPN heuristic if all else fails
                        return ReachabilityType::RI_VPN;
                    }

                    // Didn't meet any of our VPN heuristics. Let's see if the peer address is on-link.
                    Q_ASSERT(addr.prefixLength() >= 0);
                    if (addr.prefixLength() >= 0 && s.localAddress().isInSubnet(s.peerAddress(), addr.prefixLength())) {
                        return ReachabilityType::RI_LAN;
                    }

                    // Default to unknown if nothing else matched
                    return ReachabilityType::RI_UNKNOWN;
                }
            }
        }

        qWarning() << "No match found for address:" << s.localAddress();
        return ReachabilityType::RI_UNKNOWN;
    }
    else {
        // If we fail to connect, just pretend that it's not a VPN
        qWarning() << "Unable to check for reachability within 3 seconds";
        return ReachabilityType::RI_UNKNOWN;
    }
}

bool NvComputer::updateAppList(QVector<NvApp> newAppList) {
    if (appList == newAppList) {
        return false;
    }

    // Propagate client-side attributes to the new app list
    for (const NvApp& existingApp : std::as_const(appList)) {
        for (NvApp& newApp : newAppList) {
            if (existingApp.id == newApp.id) {
                newApp.hidden = existingApp.hidden;
                newApp.directLaunch = existingApp.directLaunch;
            }
        }
    }

    appList = newAppList;
    sortAppList();
    return true;
}

QVector<NvAddress> NvComputer::uniqueAddresses() const
{
    QReadLocker readLocker(&lock);
    return addressesUnlocked();
}

QVector<NvAddress> NvComputer::addressesUnlocked() const
{
    QVector<NvAddress> uniqueAddressList;

    // Start with addresses correctly ordered
    uniqueAddressList.append(activeAddress);
    uniqueAddressList.append(localAddress);
    if (NetLinkFilter::kEnabled) {
        // Every other address the host advertised over mDNS. These are ranked
        // below the working address, but above the WAN address, because a
        // multi-NIC host's extra addresses are still far better than the
        // Internet.
        for (const NvAddress& mdnsAddress : std::as_const(mdnsAddresses)) {
            uniqueAddressList.append(mdnsAddress);
        }
        // Literals resolved from a hostname the user typed. These sit ABOVE
        // remoteAddress and manualAddress on purpose: the name they replace is
        // last precisely because it is the weakest address we have, but once
        // it has been resolved we want the poller to probe the literal FIRST.
        // That is what makes activeAddress settle on a literal, and the RTSP
        // pin in Session::startConnectionAsync() rewrites the stream host to
        // activeAddress - a hostname there would make the pin a no-op and the
        // mask unenforced for the media path, which is where the bandwidth is.
        for (const NvAddress& resolvedAddress : std::as_const(resolvedAddresses)) {
            uniqueAddressList.append(resolvedAddress);
        }
    }
    uniqueAddressList.append(remoteAddress);
    uniqueAddressList.append(ipv6Address);
    uniqueAddressList.append(manualAddress);

    // Prune duplicates (always giving precedence to the first)
    for (int i = 0; i < uniqueAddressList.count(); i++) {
        if (uniqueAddressList[i].isNull()) {
            uniqueAddressList.remove(i);
            i--;
            continue;
        }
        for (int j = i + 1; j < uniqueAddressList.count(); j++) {
            if (uniqueAddressList[i] == uniqueAddressList[j]) {
                // Always remove the later occurrence
                uniqueAddressList.remove(j);
                j--;
            }
        }
    }

    // We must have at least 1 address
    Q_ASSERT(!uniqueAddressList.isEmpty());

    return uniqueAddressList;
}

QVector<NvAddress> NvComputer::allowedAddresses() const
{
    if (!NetLinkFilter::kEnabled) {
        // Fallback: the original, unfiltered behavior
        return uniqueAddresses();
    }

    quint32 allowed;
    QString hostName;
    bool alreadyLogged;
    {
        // Take the mask first, since uniqueAddresses() acquires the same lock
        // and CopySafeReadWriteLock is not recursive. Snapshot everything we
        // need for the fallback warning here so we never touch member state
        // without the lock (allowedAddresses() runs on the polling thread
        // while renameHost()/setHostConnectionOptions() may write concurrently).
        QReadLocker readLocker(&lock);
        allowed = allowedLinkTypes;
        hostName = name;
        alreadyLogged = linkFilterFallbackLogged;
    }

    // Nothing is being filtered, so skip the route probes entirely. This is
    // the state of every host the user never opened the PC settings page for,
    // and allowedAddresses() runs from the polling loop every 3 seconds for
    // every address we know about, where each probe costs a UDP connect plus
    // two QNetworkInterface::allInterfaces() enumerations.
    if (allowed == NLT_ALL) {
        return uniqueAddresses();
    }

    QVector<NvAddress> allowedAddressList;
    QVector<NvAddress> unclassifiable;

    // A hostname cannot be probed, so its verdict can only be reached once we
    // know what the literals we DO have to judge came out as. Collecting the
    // names separately keeps the literal order untouched, and keeps a name
    // from outranking a literal in the probe order.
    bool sawLiteral = false;

    for (const NvAddress& address : uniqueAddresses()) {
        if (!netAddressIsLiteral(address.address())) {
            unclassifiable.append(address);
            continue;
        }

        sawLiteral = true;

        const QVector<NetRoute> routes = netRoutesTo(QHostAddress(address.address()), address.port());
        if (bestAllowedRoute(routes, allowed).isUsable()) {
            allowedAddressList.append(address);
        }
    }

    for (const NvAddress& address : std::as_const(unclassifiable)) {
        // A name is kept ONLY while there is nothing to judge it against. Once
        // we hold a literal for this host, the literal is the real address and
        // the name is merely standing in for it, so it must not be able to
        // resurrect a route the mask has already rejected - otherwise a name
        // that resolves to nothing but a Wi-Fi address would keep a Wi-Fi-only
        // host online under a wired-only mask, which is the one thing this
        // filter exists to prevent.
        //
        // Note that the verdict does not depend on whether any literal
        // survived: an approved literal that is not answering is not a reason to
        // reach for the name either. The name resolves again at connect time and
        // would answer on whatever it points at then, so using it here is the
        // same hole with extra steps - and a worse one, because addressesUnlocked()
        // puts activeAddress first, so a name reached this way outranks the
        // literal it was standing in for on every later poll round and the RTSP
        // pin in Session::startConnectionAsync() never narrows anything again.
        //
        // With no literal at all there is nothing to judge, and dropping the
        // name would strand the host for good: the poller only probes what this
        // function returns, so an empty list means it never gets another
        // chance to learn otherwise. That is the case getUnclassifiableAddress()
        // reports, and reaching for reachability is the right side of the
        // trade. It is also the transient case while a resolution started by
        // setHostConnectionOptions() is still in flight, so erring open keeps
        // the few seconds before the literals land from looking like an outage.
        if (sawLiteral) {
            continue;
        }

        allowedAddressList.append(address);
    }

    if (allowedAddressList.isEmpty()) {
        // Every address we hold for this host is a literal and none of them has
        // any route over a connection type the user allows. The name, if there is
        // one, was dropped above along with them, so this is the mask genuinely
        // excluding the host, not us failing to classify it, and the warning
        // below is accurate. Handing the poller the unfiltered list
        // instead would latch an address that is only reachable over a
        // connection type the user excluded, putting that type back in play for
        // the control channel and the RTSP pin in
        // Session::startConnectionAsync(); an empty list simply takes the host
        // CS_OFFLINE in the polling loop, and re-allowing a type makes the next
        // round pick the host back up with no extra bookkeeping here.
        //
        // Only warn once per host until the mask changes, since this runs from
        // the polling loop every few seconds.
        bool shouldLog = false;
        {
            QWriteLocker writeLocker(&lock);
            if (!linkFilterFallbackLogged) {
                linkFilterFallbackLogged = true;
                shouldLog = true;
            }
        }
        if (shouldLog) {
            // Use the snapshot taken under the lock above. alreadyLogged is
            // only kept for symmetry with the previous logic and to avoid
            // an extra lock round-trip on the common path.
            Q_UNUSED(alreadyLogged);
            qWarning() << qPrintable(hostName) << "has no address reachable over a connection type that"
                       << "is allowed for this PC. Treating it as offline.";
        }
        return QVector<NvAddress>();
    }

    return allowedAddressList;
}

QString NvComputer::getConflictingLinkDescription() const
{
    if (!NetLinkFilter::kEnabled) {
        // Fallback: the original behavior never reported conflicts
        return QString();
    }

    quint32 allowed;
    NvAddress active;
    {
        QReadLocker readLocker(&lock);
        allowed = allowedLinkTypes;
        active = activeAddress;
    }

    if (active.isNull()) {
        return QString();
    }

    const QVector<NetRoute> routes = netRoutesTo(QHostAddress(active.address()), active.port());

    // Reaching a host whose routes are all disallowed is not a routing
    // conflict to report: the mask is a hard gate, so activeAddress could only
    // be non-null here if some route to it is allowed. Anything else means the
    // OS picked a different adapter than the one we would, which is the case
    // worth warning about - two adapters on-link for the same host, where the
    // destination address cannot select the interface.
    if (!bestAllowedRoute(routes, allowed).isUsable()) {
        return QString();
    }

    for (const NetRoute& route : std::as_const(routes)) {
        if (route.isPreferred && !isNetLinkTypeAllowed(route.bit(), allowed)) {
            return route.nicDescription;
        }
    }

    return QString();
}

NvComputer::LinkFilterProbe NvComputer::probeLinkFilter() const
{
    LinkFilterProbe probe;

    if (!NetLinkFilter::kEnabled) {
        // Fallback: the original behavior reported neither of these
        return probe;
    }

    quint32 allowed;
    QVector<NvAddress> addresses;
    bool online;
    {
        // One snapshot for all three reads, since addressesUnlocked() has to be
        // called without the lock held - CopySafeReadWriteLock is not recursive.
        QReadLocker readLocker(&lock);

        // NLT_ALL filters nothing, so there is no guarantee to be missing and
        // no address to have excluded.
        if (allowedLinkTypes == NLT_ALL) {
            return probe;
        }

        allowed = allowedLinkTypes;
        addresses = addressesUnlocked();
        online = (state == CS_ONLINE);
    }

    for (const NvAddress& address : std::as_const(addresses)) {
        if (!netAddressIsLiteral(address.address())) {
            // Note the first name we see, but keep looking: a literal further
            // down that the mask approves is not in the same state as this one,
            // and the two are reported differently.
            if (probe.name.isEmpty()) {
                probe.name = address.address();
            }
            continue;
        }

        probe.sawLiteral = true;

        const QVector<NetRoute> routes = netRoutesTo(QHostAddress(address.address()), address.port());

        if (bestAllowedRoute(routes, allowed).isUsable()) {
            // Something the mask approves is reachable. Note that "reachable"
            // here is about the routing table, not about the host answering: the
            // address can have a perfectly good wired route and still be an
            // address the PC has moved away from, which is why the name cannot
            // simply be kept as a fallback for it.
            probe.anyLiteralAllowed = true;
        }

        // Every type this address can be reached over, kept apart from the
        // verdict above: a route with no source address is no way to reach the
        // host at all, and must not be offered to the user as a connection type
        // they could have selected. Only consulted when the verdict came out
        // negative, where the whole set is deselected by definition.
        for (const NetRoute& route : routes) {
            if (route.isUsable()) {
                probe.reachableTypes |= route.bit();
            }
        }
    }

    probe.hostOnline = online;

    return probe;
}

QString NvComputer::getUnclassifiableAddress() const
{
    const LinkFilterProbe probe = probeLinkFilter();

    // Only the state where we hold no literal to judge the name against, which is
    // exactly the state where allowedAddresses() keeps the name and the poller
    // is about to use it. Once a literal has landed the name is no longer in the
    // candidate list at all, so nothing is going to be probed or streamed over it
    // and describing a stream that cannot happen would be the opposite of the
    // truth. Those two states are getMaskExcludedLinkTypes()'s and
    // getNameSuppressedAddress()'s to report, one for each of the ways a literal
    // can leave the host unreachable.
    if (probe.sawLiteral) {
        return QString();
    }

    // The mask cannot be enforced on a name and the poller is about to use it,
    // and nothing downstream will say so - the RTSP pin in
    // Session::startConnectionAsync() rewrites to the same string it already
    // had. Reporting it here is the only way the user finds out.
    return probe.name;
}

quint32 NvComputer::getMaskExcludedLinkTypes() const
{
    const LinkFilterProbe probe = probeLinkFilter();

    // The complement of getUnclassifiableAddress(): the name is only reachable
    // while there is no literal, and this is the state where one exists and the
    // mask turned down every one of them. allowedAddresses() drops the name
    // here, so the poller gets an empty candidate list and takes the host
    // CS_OFFLINE - the mask is doing exactly what the user asked, and the only
    // thing left to do is tell them which type to re-allow.
    if (!probe.sawLiteral || probe.anyLiteralAllowed) {
        return 0;
    }

    return probe.reachableTypes;
}

QString NvComputer::getNameSuppressedAddress() const
{
    const LinkFilterProbe probe = probeLinkFilter();

    // The third state, and the one the mask cannot repair on its own: a literal
    // exists and the mask approved it, so the poller is probing an address the
    // user allowed, and it is not answering. The name would answer - it resolves
    // again at connect time and would go wherever DNS points then - but using
    // it here is the hole this whole feature exists to close, so it is not used.
    //
    // Requiring anyLiteralAllowed is what separates this from the mask-excluded
    // case: there the mask itself is the reason nothing is reachable and the
    // remedy is to select another connection type, here the mask is working
    // exactly as asked and the approved address is simply stale, asleep, or
    // gone. Reporting them with the same words would send the user to fix the
    // wrong thing.
    if (probe.name.isEmpty() || !probe.sawLiteral || !probe.anyLiteralAllowed) {
        return QString();
    }

    // Online through the name would mean allowedAddresses() still had it, which
    // is the state above. So reaching here with the host online would be a
    // contradiction rather than a fact worth showing, and the ordinary
    // "nothing is wrong" answer is the honest one.
    if (probe.hostOnline) {
        return QString();
    }

    return probe.name;
}

bool NvComputer::hasNameAddress() const
{
    QReadLocker readLocker(&lock);

    // Only these two can be a name: manualAddress is whatever the user typed
    // into addNewHostManually(), and activeAddress can hold a name only while
    // the poller had nothing else to latch - which, once allowedAddresses()
    // drops names as soon as a literal exists, is the same window in which we
    // hold no literal at all. The rest are literals by construction: mdns
    // announces IPs, localAddress comes from the host's own LocalIP, and
    // remoteAddress/ipv6Address are parsed addresses.
    const NvAddress& manual = manualAddress;
    const NvAddress& active = activeAddress;

    return (!manual.isNull() && !netAddressIsLiteral(manual.address())) ||
           (!active.isNull() && !netAddressIsLiteral(active.address()));
}

bool NvComputer::update(const NvComputer& that)
{
    bool changed = false;

    // Whether to accept that.activeAddress depends on a routing probe, and
    // netRoutesTo() blocks on a UDP connect (up to a second) plus two
    // QNetworkInterface::allInterfaces() enumerations. That is far too much
    // work to do while holding the write lock below, which every read of this
    // host contends with: the GUI model roles, getConflictingLinkDescription(),
    // the poller's allowedAddresses() and updateAppList(), and
    // setHostConnectionOptions(). So the probe runs first, unlocked, and the
    // write lock is only taken once its inputs are known.
    //
    // that is safe to read here because every caller passes a local object that
    // no other thread can reach (computermanager.cpp passes newState,
    // httpsComputer and newComputer). allowedLinkTypes is genuine shared state
    // written from the GUI thread, so it is snapshotted and re-validated below.
    NvAddress probedAddress;
    quint32 probedMask = 0;
    bool probeNeeded = false;
    {
        QReadLocker readLock(&lock);
        QReadLocker thatReadLock(&that.lock);

        // UUID may not change or we're talking to a new PC
        Q_ASSERT(this->uuid == that.uuid);

        // The probe is only worth its cost when the value would actually change:
        // the polling thread re-latches the same address every round, and a host
        // that allows every connection type has nothing to filter in the first
        // place.
        //
        // A hostname is excluded from the probe entirely. netRoutesTo() cannot
        // classify one, so its verdict would always be "no allowed route" and
        // the address could never be latched - which for a host the user added
        // by name is the same permanent-offline bug allowedAddresses() has, and
        // the worse of the two, since nothing re-probes a name. Latching it
        // instead leaves the mask unenforced on this host, which
        // getUnclassifiableAddress() reports; that is strictly better than
        // never reaching the host again.
        //
        // This is consistent with allowedAddresses() dropping names as soon as
        // a literal exists: we only reach here with a name to accept in the
        // window where we hold no literal at all, so latching one can never
        // outrank a literal that the mask approved.
        if (NetLinkFilter::kEnabled && this->allowedLinkTypes != NLT_ALL &&
                this->activeAddress != that.activeAddress && !that.activeAddress.isNull() &&
                netAddressIsLiteral(that.activeAddress.address())) {
            probeNeeded = true;
            probedAddress = that.activeAddress;
            probedMask = this->allowedLinkTypes;
        }
    }

    // True means the address may be latched; a false verdict keeps the address
    // we already had. Note that allowedAddresses() cannot be used for the same
    // job, since CopySafeReadWriteLock is not recursive and we do not hold the
    // lock at this point anyway.
    bool probedAddressAllowed = true;
    if (probeNeeded) {
        const QVector<NetRoute> routes =
            netRoutesTo(QHostAddress(probedAddress.address()), probedAddress.port());
        probedAddressAllowed = bestAllowedRoute(routes, probedMask).isUsable();
    }

    // Lock us for write and them for read
    QWriteLocker thisLock(&this->lock);
    QReadLocker thatLock(&that.lock);

#define ASSIGN_IF_CHANGED(field)       \
    if (this->field != that.field) {   \
        this->field = that.field;      \
        changed = true;                \
    }

#define ASSIGN_IF_CHANGED_AND_NONEMPTY(field) \
    if (!that.field.isEmpty() &&              \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

#define ASSIGN_IF_CHANGED_AND_NONNULL(field)  \
    if (!that.field.isNull() &&               \
        this->field != that.field) {          \
        this->field = that.field;             \
        changed = true;                       \
    }

    if (!hasCustomName) {
        // Only overwrite the name if it's not custom
        ASSIGN_IF_CHANGED(name);
    }
    ASSIGN_IF_CHANGED_AND_NONEMPTY(macAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(localAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(remoteAddress);
    ASSIGN_IF_CHANGED_AND_NONNULL(ipv6Address);
    ASSIGN_IF_CHANGED_AND_NONNULL(manualAddress);
    // Refreshed on every mDNS announcement so newly advertised addresses
    // become selectable. This must be "AND_NONEMPTY" because a host polled
    // over the control port has no mDNS addresses at all, and blindly copying
    // that empty list would discard the ones mDNS discovered.
    // NB: allowedLinkTypes is deliberately NOT assigned here, since it is
    // user-owned state.
    ASSIGN_IF_CHANGED_AND_NONEMPTY(mdnsAddresses);
    ASSIGN_IF_CHANGED(activeHttpsPort);
    ASSIGN_IF_CHANGED(externalPort);
    ASSIGN_IF_CHANGED(pairState);
    ASSIGN_IF_CHANGED(serverCodecModeSupport);
    ASSIGN_IF_CHANGED(currentGameId);

    // The mDNS re-resolution path folds a freshly discovered NvComputer into
    // this one (see ComputerManager::handleMdnsServiceResolved), and its
    // activeAddress is simply the first address the host advertised, which the
    // per-PC connection type filter may well exclude. Latching it anyway would
    // let a disallowed address reach both the HTTPS control channel and the
    // RTSP pin in Session::startConnectionAsync(), so keep the address we
    // already had and let the poller re-evaluate the filtered list instead.
    //
    // The per-PC mask gates addresses, not adapters. An address with no route
    // over a connection type the user allows is never latched, even when it
    // is the only one that answers: losing the host is far better than letting
    // an excluded connection type answer for it. Once no address qualifies,
    // allowedAddresses() returns an empty candidate list and the polling loop
    // takes the host CS_OFFLINE, which is the intended reading rather than a
    // failure to report.
    //
    // "An address" here means a literal. A hostname carries no route
    // information at all, so this gate has nothing to say about it either way
    // and lets it through - see the probe condition above for why that is the
    // lesser evil, and getUnclassifiableAddress() for how the user finds out.
    //
    // The same predicate cannot promise which adapter the traffic leaves
    // through. bestAllowedRoute() only asks whether SOME route is allowed, and
    // when two adapters share the host's subnet the destination address cannot
    // select between them, so the OS interface metric may still pick the
    // excluded one. That is exactly the case getConflictingLinkDescription()
    // reports, and it is the only backstop: nothing in the connection path pins
    // the outgoing interface, because moonlight-common-c derives its source
    // address from the kernel itself (getLocalAddressByUdpConnect in
    // Connection.c) and NvHTTP binds no local address either.
    //
    // The verdict was computed above from a snapshot of allowedLinkTypes, which
    // setHostConnectionOptions() can rewrite from the GUI thread at any time. If
    // the mask moved while the probe was running, the answer describes a mask
    // that no longer exists, so it is discarded and the old address kept. That
    // is the conservative direction for a mask that was narrowed, and for one
    // that was widened it costs nothing: setHostConnectionOptions() clears
    // activeAddress anyway, and the poller re-evaluates within one round.
    bool acceptActiveAddress = true;
    if (probeNeeded) {
        acceptActiveAddress = (this->allowedLinkTypes == probedMask) && probedAddressAllowed;
    }
    if (acceptActiveAddress) {
        ASSIGN_IF_CHANGED(activeAddress);
    }

    // CS_ONLINE without an address is not a state anything downstream can
    // use: Session::startConnectionAsync() and the poller's updateAppList()
    // both build an NvHTTP from activeAddress, which asserts on a null one
    // and otherwise produces a URL with no host. So the "online" verdict has
    // to follow the address we actually accepted, rather than being taken
    // unconditionally. Leaving the previous state in place is enough to make
    // the host re-evaluate: the poller runs again in a few seconds.
    //
    // Under the hard-gate mask this is reachable only via the mDNS fold above,
    // since the poller can never offer an address the filter rejected. It stays
    // as the second line of defense rather than relying on that invariant.
    if (!(that.state == NvComputer::CS_ONLINE && this->activeAddress.isNull())) {
        ASSIGN_IF_CHANGED(state);
    }
    ASSIGN_IF_CHANGED(gfeVersion);
    ASSIGN_IF_CHANGED(appVersion);
    ASSIGN_IF_CHANGED(isSupportedServerVersion);
    ASSIGN_IF_CHANGED(isNvidiaServerSoftware);
    ASSIGN_IF_CHANGED(maxLumaPixelsHEVC);
    ASSIGN_IF_CHANGED(gpuModel);
    ASSIGN_IF_CHANGED_AND_NONNULL(serverCert);
    ASSIGN_IF_CHANGED_AND_NONEMPTY(displayModes);

    if (!that.appList.isEmpty()) {
        // updateAppList() handles merging client-side attributes
        updateAppList(that.appList);
    }

    return changed;
}
