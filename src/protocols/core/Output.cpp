#include "Output.hpp"
#include "Compositor.hpp"
#include "../../Compositor.hpp"
#include "../../output/Monitor.hpp"
#include "../../managers/eventLoop/EventLoopManager.hpp"

CWLOutputResource::CWLOutputResource(SP<CWlOutput> resource_, PHLMONITOR pMonitor, WP<CWLOutputProtocol> owner) : m_monitor(pMonitor), m_owner(owner), m_resource(resource_) {
    if UNLIKELY (!good())
        return;

    m_resource->setData(this);

    m_client = m_resource->client();

    // route through the owning protocol object, not a name lookup: after a monitor
    // unplug/replug PROTO::outputs.at(name) is a *new* global, and releases meant for the
    // old defunct one would never drain it.
    m_resource->setOnDestroy([this](CWlOutput* r) {
        if (m_owner)
            m_owner->destroyResource(this);
    });
    m_resource->setRelease([this](CWlOutput* r) {
        if (m_owner)
            m_owner->destroyResource(this);
    });

    if (!m_monitor || !m_monitor->m_output || !m_owner || m_owner->m_defunct)
        return;

    if (m_resource->version() >= 4) {
        m_resource->sendName(m_monitor->m_name.c_str());
        m_resource->sendDescription(m_monitor->m_description.c_str());
    }

    updateState();

    const auto PMONITOR = m_monitor.lock();
    if (!PMONITOR)
        return;

    PROTO::compositor->forEachSurface([PMONITOR](SP<CWLSurfaceResource> surf) {
        auto HLSurf = Desktop::View::CWLSurface::fromResource(surf);

        if (!HLSurf)
            return;

        const auto GEOMETRY = HLSurf->getSurfaceBoxGlobal();

        if (!GEOMETRY.has_value())
            return;

        if (!PMONITOR->logicalBox().expand(-4).overlaps(*GEOMETRY))
            return;

        surf->enter(PMONITOR);
    });
}

SP<CWLOutputResource> CWLOutputResource::fromResource(wl_resource* res) {
    auto data = sc<CWLOutputResource*>(sc<CWlOutput*>(wl_resource_get_user_data(res))->data());
    return data ? data->m_self.lock() : nullptr;
}

bool CWLOutputResource::good() {
    return m_resource->resource();
}

wl_client* CWLOutputResource::client() {
    return m_client;
}

SP<CWlOutput> CWLOutputResource::getResource() {
    return m_resource;
}

void CWLOutputResource::updateState() {
    if (!m_monitor || !m_monitor->m_output || !m_owner || m_owner->m_defunct)
        return;

    if (m_resource->version() >= 2)
        m_resource->sendScale(std::ceil(m_monitor->m_scale));

    m_resource->sendMode(WL_OUTPUT_MODE_CURRENT, m_monitor->m_pixelSize.x, m_monitor->m_pixelSize.y, m_monitor->m_refreshRate * 1000.0);

    m_resource->sendGeometry(m_monitor->m_position.x, m_monitor->m_position.y, m_monitor->m_output->physicalSize.x, m_monitor->m_output->physicalSize.y,
                             m_monitor->m_output->subpixel, m_monitor->m_output->make.c_str(), m_monitor->m_output->model.c_str(), m_monitor->m_transform);

    if (m_resource->version() >= 2)
        m_resource->sendDone();
}

CWLOutputProtocol::CWLOutputProtocol(const wl_interface* iface, const int& ver, const std::string& name, PHLMONITOR pMonitor) :
    IWaylandProtocol(iface, ver, name), m_monitor(pMonitor), m_name(pMonitor->m_name) {

    m_listeners.modeChanged = m_monitor->m_events.modeChanged.listen([this] {
        for (auto const& o : m_outputs) {
            o->updateState();
        }
    });
}

CWLOutputProtocol::~CWLOutputProtocol() {
    if (m_removeTimer && g_pEventLoopManager)
        g_pEventLoopManager->removeTimer(m_removeTimer);
}

void CWLOutputProtocol::bindManager(wl_client* client, void* data, uint32_t ver, uint32_t id) {
    const auto RESOURCE = m_outputs.emplace_back(makeShared<CWLOutputResource>(makeShared<CWlOutput>(client, ver, id), m_monitor.lock(), m_self));

    if UNLIKELY (!RESOURCE->good()) {
        wl_client_post_no_memory(client);
        m_outputs.pop_back();
        return;
    }

    RESOURCE->m_self = RESOURCE;

    if (m_defunct || !m_monitor || !m_monitor->m_output)
        return;

    m_events.outputBound.emit(RESOURCE);
}

void CWLOutputProtocol::destroyResource(CWLOutputResource* resource) {
    std::erase_if(m_outputs, [&](const auto& other) { return other.get() == resource; });

    // NOTE: may drop the last ref to `this` — nothing may run after it.
    dropIfUnused();
}

void CWLOutputProtocol::dropIfUnused() {
    if (!m_outputs.empty() || !m_defunct || m_removeTimer)
        return;

    // only erase the map entry if it is actually us: a new global with the same name may
    // have replaced us (monitor replug), in which case we live in defunctOutputs.
    // NOTE: either erase drops the last ref to `this` — nothing may run after it.
    if (PROTO::outputs.contains(m_name) && PROTO::outputs.at(m_name).get() == this)
        PROTO::outputs.erase(m_name);
    else
        std::erase_if(PROTO::defunctOutputs, [this](const auto& other) { return other.get() == this; });
}

bool CWLOutputProtocol::hasBoundResources() {
    return !m_outputs.empty();
}

std::vector<SP<CWLOutputResource>> CWLOutputProtocol::outputResourcesFrom(wl_client* client) {
    std::vector<SP<CWLOutputResource>> ret;

    for (auto const& r : m_outputs) {
        if (r->client() != client)
            continue;

        ret.emplace_back(r);
    }

    return ret;
}

void CWLOutputProtocol::remove() {
    if UNLIKELY (m_defunct)
        return;

    m_defunct = true;
    removeGlobal();

    // wl_global_remove only tells clients the global is going away. One that has not read
    // that yet can still be binding it, and binding a destroyed global is a protocol error
    // that fatally disconnects the client. Every client binds a new wl_output as soon as it
    // is announced, so a monitor that comes and goes within a roundtrip (a flapping connector
    // or a quick replug) would take every client down. Keep the global alive long enough for
    // those binds to land, as wlroots does.
    m_removeTimer = makeShared<CEventLoopTimer>(
        std::chrono::seconds(5),
        [self = m_self](SP<CEventLoopTimer> timer, void* data) {
            const auto PSELF = self.lock();
            if (!PSELF)
                return;

            g_pEventLoopManager->removeTimer(timer);
            PSELF->m_removeTimer.reset();
            PSELF->dropIfUnused();
        },
        nullptr);
    g_pEventLoopManager->addTimer(m_removeTimer);
}

bool CWLOutputProtocol::isDefunct() {
    return m_defunct;
}

void CWLOutputProtocol::sendDone() {
    if UNLIKELY (m_defunct)
        return;

    for (auto const& r : m_outputs) {
        r->m_resource->sendDone();
    }
}
