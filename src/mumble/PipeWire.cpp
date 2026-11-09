// Copyright The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "PipeWire.h"

#include "Global.h"

#include <pipewire/core.h>
#include <pipewire/keys.h>
#include <pipewire/node.h>
#include <pipewire/stream.h>
#include <pipewire/type.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <vector>

#define RESOLVE(var)                                                  \
	{                                                                 \
		var = reinterpret_cast< decltype(var) >(m_lib.resolve(#var)); \
		if (!var)                                                     \
			return;                                                   \
	}

static std::unique_ptr< PipeWireSystem > pws;

class PipeWireInputRegistrar : public AudioInputRegistrar {
private:
	AudioInput *create() override;
	const QVariant getDeviceChoice() override;
	const QList< audioDevice > getDeviceChoices() override;
	void setDeviceChoice(const QVariant &, Settings &) override;
	bool canEcho(EchoCancelOptionID, const QString &) const override;
	bool isMicrophoneAccessDeniedByOS() override;

public:
	PipeWireInputRegistrar();
};

class PipeWireOutputRegistrar : public AudioOutputRegistrar {
private:
	AudioOutput *create() override;
	const QVariant getDeviceChoice() override;
	const QList< audioDevice > getDeviceChoices() override;
	void setDeviceChoice(const QVariant &, Settings &) override;
	bool usesOutputDelay() const override;
	bool canMuteOthers() const override;

public:
	PipeWireOutputRegistrar();
};

class PipeWireInit : public DeferInit {
private:
	std::unique_ptr< PipeWireInputRegistrar > inputRegistrar;
	std::unique_ptr< PipeWireOutputRegistrar > outputRegistrar;
	void initialize() override;
	void destroy() override;
};

PipeWireInputRegistrar::PipeWireInputRegistrar() : AudioInputRegistrar(QStringLiteral("PipeWire"), 100) {
}

AudioInput *PipeWireInputRegistrar::create() {
	return new PipeWireInput;
}

const QVariant PipeWireInputRegistrar::getDeviceChoice() {
	return Global::get().s.pipeWireInput;
}

const QList< audioDevice > PipeWireInputRegistrar::getDeviceChoices() {
	return { audioDevice(QStringLiteral("Mono"), 1) };
}

void PipeWireInputRegistrar::setDeviceChoice(const QVariant &choice, Settings &settings) {
	settings.pipeWireInput = static_cast< std::uint8_t >(choice.toUInt());
}

bool PipeWireInputRegistrar::canEcho(EchoCancelOptionID, const QString &) const {
	return false;
}

bool PipeWireInputRegistrar::isMicrophoneAccessDeniedByOS() {
	return false;
}

PipeWireOutputRegistrar::PipeWireOutputRegistrar() : AudioOutputRegistrar(QStringLiteral("PipeWire"), 100) {
}

AudioOutput *PipeWireOutputRegistrar::create() {
	return new PipeWireOutput;
}

const QVariant PipeWireOutputRegistrar::getDeviceChoice() {
	return Global::get().s.pipeWireOutput;
}

const QList< audioDevice > PipeWireOutputRegistrar::getDeviceChoices() {
	return { audioDevice(QStringLiteral("Mono"), 1), audioDevice(QStringLiteral("Stereo"), 2),
			 audioDevice(QStringLiteral("2.1"), 3),  audioDevice(QStringLiteral("3.1"), 4),
			 audioDevice(QStringLiteral("5.1"), 6),  audioDevice(QStringLiteral("7.1"), 8) };
}

void PipeWireOutputRegistrar::setDeviceChoice(const QVariant &choice, Settings &settings) {
	settings.pipeWireOutput = static_cast< std::uint8_t >(choice.toUInt());
}

bool PipeWireOutputRegistrar::usesOutputDelay() const {
	return false;
}

bool PipeWireOutputRegistrar::canMuteOthers() const {
	return true;
}

void PipeWireInit::initialize() {
	pws = std::make_unique< PipeWireSystem >();

	if (pws->m_ok) {
		inputRegistrar  = std::make_unique< PipeWireInputRegistrar >();
		outputRegistrar = std::make_unique< PipeWireOutputRegistrar >();
	} else {
		pws.reset();
	}
}

void PipeWireInit::destroy() {
	inputRegistrar.reset();
	outputRegistrar.reset();
	pws.reset();
}

static PipeWireInit pwi;

PipeWireSystem::PipeWireSystem() : m_ok(false), m_users(0) {
	// clang-format off
	const QStringList names {
		// Common names used by Linux distributions.
		"libpipewire.so",
		"libpipewire-0.3.so",
		// Name used by the Flatpak FreeDesktop runtime.
		"libpipewire-0.3.so.0"
	};
	// clang-format on

	for (const auto &name : names) {
		m_lib.setFileName(name);
		if (m_lib.load()) {
			break;
		}
		m_lib.unload();
	}

	if (!m_lib.isLoaded()) {
		return;
	}

	RESOLVE(pw_get_library_version);
	RESOLVE(pw_init);
	RESOLVE(pw_deinit);
	RESOLVE(pw_loop_new);
	RESOLVE(pw_loop_destroy);
	RESOLVE(pw_thread_loop_new_full);
	RESOLVE(pw_thread_loop_destroy);
	RESOLVE(pw_thread_loop_start);
	RESOLVE(pw_thread_loop_stop);
	RESOLVE(pw_thread_loop_lock);
	RESOLVE(pw_thread_loop_unlock);
	RESOLVE(pw_thread_loop_timed_wait);
	RESOLVE(pw_thread_loop_signal);
	RESOLVE(pw_context_new);
	RESOLVE(pw_context_destroy);
	RESOLVE(pw_context_connect);
	RESOLVE(pw_core_disconnect);
	RESOLVE(pw_proxy_destroy);
	RESOLVE(pw_properties_new);
	RESOLVE(pw_stream_new_simple);
	RESOLVE(pw_stream_set_active);
	RESOLVE(pw_stream_destroy);
	RESOLVE(pw_stream_connect);
	RESOLVE(pw_stream_get_node_id);
	RESOLVE(pw_stream_dequeue_buffer);
	RESOLVE(pw_stream_queue_buffer);

	qInfo("PipeWire %s from %s", pw_get_library_version(), qPrintable(m_lib.fileName()));

	pw_init(nullptr, nullptr);

	m_ok = true;
}

PipeWireSystem::~PipeWireSystem() {
	if (m_ok) {
		pw_deinit();
	}
}

PipeWireEngine::PipeWireEngine(const char *category, void *param, const std::function< void(void *param) > callback)
	: m_ok(false), m_loop(nullptr), m_stream(nullptr), m_thread(nullptr) {
	if (!pws->isOk()) {
		return;
	}

	m_loop = pws->pw_loop_new(nullptr);
	if (!m_loop) {
		return;
	}

	pw_properties *props =
		pws->pw_properties_new(PW_KEY_APP_NAME, "Mumble", PW_KEY_NODE_NAME, "Mumble", PW_KEY_MEDIA_CATEGORY, category,
							   PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_ROLE, "Communication", nullptr);

	m_events          = std::make_unique< pw_stream_events >();
	m_events->version = PW_VERSION_STREAM_EVENTS;
	m_events->process = *callback.target< void (*)(void *) >();

	m_stream = pws->pw_stream_new_simple(m_loop, category, props, m_events.get(), param);
	if (!m_stream) {
		return;
	}

	m_thread = pws->pw_thread_loop_new_full(m_loop, category, nullptr);
	if (m_thread) {
		m_ok = true;
	}
}

PipeWireEngine::~PipeWireEngine() {
	if (!pws->isOk()) {
		return;
	}

	if (m_stream) {
		pws->pw_stream_destroy(m_stream);
	}

	if (m_thread) {
		pws->pw_thread_loop_destroy(m_thread);
	}


	if (m_loop) {
		pws->pw_loop_destroy(m_loop);
	}
}

bool PipeWireEngine::connect(const uint8_t direction, const uint32_t *channels, const uint8_t nChannels) {
	uint8_t buffer[1024];

	spa_pod_builder builder{};
	builder.data = buffer;
	builder.size = sizeof(buffer);

	spa_audio_info_raw info{};
	info.channels = nChannels;
	info.format   = SPA_AUDIO_FORMAT_F32;
	info.rate     = SAMPLE_RATE;

	if (nChannels < 2) {
		info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	} else {
		for (uint8_t i = 0; i < nChannels; ++i) {
			switch (channels[i]) {
				case SPEAKER_FRONT_LEFT:
					info.position[i] = SPA_AUDIO_CHANNEL_FL;
					break;
				case SPEAKER_FRONT_RIGHT:
					info.position[i] = SPA_AUDIO_CHANNEL_FR;
					break;
				case SPEAKER_FRONT_CENTER:
					info.position[i] = SPA_AUDIO_CHANNEL_FC;
					break;
				case SPEAKER_LOW_FREQUENCY:
					info.position[i] = SPA_AUDIO_CHANNEL_LFE;
					break;
				case SPEAKER_BACK_LEFT:
					info.position[i] = SPA_AUDIO_CHANNEL_RL;
					break;
				case SPEAKER_BACK_RIGHT:
					info.position[i] = SPA_AUDIO_CHANNEL_RR;
					break;
				case SPEAKER_SIDE_LEFT:
					info.position[i] = SPA_AUDIO_CHANNEL_SL;
					break;
				case SPEAKER_SIDE_RIGHT:
					info.position[i] = SPA_AUDIO_CHANNEL_SR;
					break;
				default:
					qWarning("PipeWireSystem: Unhandled standard channel mapping 0x%x!", channels[i]);
			}
		}
	}

	const spa_pod *params[1] = { spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info) };

	return pws->pw_stream_connect(m_stream, direction, PW_ID_ANY,
								  PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS,
								  params, 1)
		   == 0;
}

void PipeWireEngine::start() {
	if (m_thread) {
		pws->pw_thread_loop_start(m_thread);
	}
}

void PipeWireEngine::stop() {
	if (m_thread) {
		pws->pw_thread_loop_stop(m_thread);
	}
}

pw_buffer *PipeWireEngine::dequeueBuffer() {
	if (m_stream) {
		return pws->pw_stream_dequeue_buffer(m_stream);
	}

	return nullptr;
}

void PipeWireEngine::queueBuffer(pw_buffer *buffer) {
	if (m_stream) {
		pws->pw_stream_queue_buffer(m_stream, buffer);
	}
}

void PipeWireEngine::setActive(const bool active) {
	pws->pw_thread_loop_lock(m_thread);
	pws->pw_stream_set_active(m_stream, active);
	pws->pw_thread_loop_unlock(m_thread);
}

uint32_t PipeWireEngine::nodeID() {
	return pws->pw_stream_get_node_id(m_stream);
}

/// Attenuates the playback streams of other applications by lowering their volume.
///
/// All callbacks run on the engine's thread loop. The decision whether to attenuate is made in the realtime process
/// callback, which only publishes it through an atomic that a timer polls.
class PipeWireAttenuator {
public:
	PipeWireAttenuator(PipeWireEngine &engine, const std::atomic< bool > &requested);
	~PipeWireAttenuator();

private:
	struct Node {
		pw_proxy *proxy;
		spa_hook listener;
		/// As last reported by the server or set by us.
		std::vector< float > volumes;
		std::vector< float > normalVolumes;
		std::vector< float > attenuatedVolumes;
		bool attenuated;
		/// Part of an internal pipeline (e.g. a loopback), so its audio comes from another node.
		bool loopback;
	};

	struct Link {
		uint32_t outputNode;
		uint32_t inputNode;
	};

	PipeWireEngine &m_engine;
	const std::atomic< bool > &m_requested;

	pw_context *m_context;
	pw_core *m_core;
	pw_registry *m_registry;
	spa_source *m_timer;

	spa_hook m_coreListener;
	spa_hook m_registryListener;
	pw_core_events m_coreEvents;
	pw_registry_events m_registryEvents;
	pw_node_events m_nodeEvents;

	std::unordered_map< uint32_t, std::unique_ptr< Node > > m_nodes;
	std::unordered_map< uint32_t, Link > m_links;
	bool m_attenuating;
	/// Links changed since the attenuated streams were last determined.
	bool m_routingChanged;
	int m_syncSeq;
	bool m_synced;

	void setAttenuating(bool attenuating);
	bool shouldAttenuate(uint32_t id, const Node &node, const std::vector< uint32_t > &ownSinks) const;
	std::vector< uint32_t > sinksOf(uint32_t nodeID) const;
	static void setVolumes(Node &node, const std::vector< float > &volumes);

	static void onCoreDone(void *data, uint32_t id, int seq);
	static void onGlobal(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version,
						 const spa_dict *props);
	static void onGlobalRemove(void *data, uint32_t id);
	static void onNodeInfo(void *data, const pw_node_info *info);
	static void onNodeParam(void *data, int seq, uint32_t id, uint32_t index, uint32_t next, const spa_pod *param);
	static void onTimer(void *data, uint64_t expirations);

	Q_DISABLE_COPY(PipeWireAttenuator)
};

PipeWireAttenuator::PipeWireAttenuator(PipeWireEngine &engine, const std::atomic< bool > &requested)
	: m_engine(engine), m_requested(requested), m_context(nullptr), m_core(nullptr), m_registry(nullptr),
	  m_timer(nullptr), m_coreListener(), m_registryListener(), m_coreEvents(), m_registryEvents(), m_nodeEvents(),
	  m_attenuating(false), m_routingChanged(false), m_syncSeq(0), m_synced(false) {
	m_context = pws->pw_context_new(m_engine.m_loop, nullptr, 0);
	if (!m_context) {
		return;
	}

	m_core = pws->pw_context_connect(m_context, nullptr, 0);
	if (!m_core) {
		return;
	}

	m_coreEvents.version = PW_VERSION_CORE_EVENTS;
	m_coreEvents.done    = onCoreDone;
	pw_core_add_listener(m_core, &m_coreListener, &m_coreEvents, this);

	m_registry = pw_core_get_registry(m_core, PW_VERSION_REGISTRY, 0);
	if (!m_registry) {
		return;
	}

	m_registryEvents.version       = PW_VERSION_REGISTRY_EVENTS;
	m_registryEvents.global        = onGlobal;
	m_registryEvents.global_remove = onGlobalRemove;
	pw_registry_add_listener(m_registry, &m_registryListener, &m_registryEvents, this);

	m_nodeEvents.version = PW_VERSION_NODE_EVENTS;
	m_nodeEvents.info    = onNodeInfo;
	m_nodeEvents.param   = onNodeParam;

	m_timer = pw_loop_add_timer(m_engine.m_loop, onTimer, this);
	if (m_timer) {
		timespec interval{};
		interval.tv_nsec = 50 * 1000 * 1000;
		pw_loop_update_timer(m_engine.m_loop, m_timer, &interval, &interval, false);
	}
}

PipeWireAttenuator::~PipeWireAttenuator() {
	if (!m_context) {
		return;
	}

	pws->pw_thread_loop_lock(m_engine.m_thread);

	if (m_timer) {
		pw_loop_destroy_source(m_engine.m_loop, m_timer);
	}

	if (m_attenuating) {
		setAttenuating(false);

		// Make sure the restored volumes reach the server before we disconnect.
		m_syncSeq = pw_core_sync(m_core, PW_ID_CORE, 0);
		while (!m_synced) {
			if (pws->pw_thread_loop_timed_wait(m_engine.m_thread, 1) != 0) {
				qWarning("PipeWireAttenuator: Timed out while restoring volumes");
				break;
			}
		}
	}

	for (auto &entry : m_nodes) {
		spa_hook_remove(&entry.second->listener);
		pws->pw_proxy_destroy(entry.second->proxy);
	}
	m_nodes.clear();

	if (m_registry) {
		spa_hook_remove(&m_registryListener);
		pws->pw_proxy_destroy(reinterpret_cast< pw_proxy * >(m_registry));
	}

	if (m_core) {
		spa_hook_remove(&m_coreListener);
		pws->pw_core_disconnect(m_core);
	}

	pws->pw_context_destroy(m_context);

	pws->pw_thread_loop_unlock(m_engine.m_thread);
}

void PipeWireAttenuator::setAttenuating(const bool attenuating) {
	// Which streams to attenuate depends on the routing, e.g. when our stream was moved to another device.
	if (attenuating == m_attenuating && !(attenuating && m_routingChanged)) {
		return;
	}

	const uint32_t ownID                   = m_engine.nodeID();
	const std::vector< uint32_t > ownSinks = sinksOf(ownID);

	// While our stream is being moved, it is briefly not linked to any sink. Keep the current state instead of
	// restoring and attenuating again a moment later.
	if (attenuating && m_attenuating && ownSinks.empty()) {
		return;
	}

	m_attenuating    = attenuating;
	m_routingChanged = false;
	// fOtherVolume is meant for cubic volumes (as used by PulseAudio), while channelVolumes are linear.
	const float factor = std::pow(Global::get().s.fOtherVolume, 3.0f);

	for (auto &entry : m_nodes) {
		Node &node = *entry.second;

		const bool attenuate = attenuating && entry.first != ownID && !node.volumes.empty()
							   && shouldAttenuate(entry.first, node, ownSinks);

		if (attenuate && !node.attenuated) {
			node.normalVolumes     = node.volumes;
			node.attenuatedVolumes = node.volumes;
			for (float &volume : node.attenuatedVolumes) {
				volume *= factor;
			}

			setVolumes(node, node.attenuatedVolumes);
			node.attenuated = true;
		} else if (!attenuate && node.attenuated) {
			node.attenuated = false;

			// Leave the volume alone if it was changed while we were attenuating.
			const bool unchanged =
				std::equal(node.volumes.cbegin(), node.volumes.cend(), node.attenuatedVolumes.cbegin(),
						   node.attenuatedVolumes.cend(), [](float a, float b) { return std::abs(a - b) < 1e-5f; });
			if (unchanged) {
				setVolumes(node, node.normalVolumes);
			}
		}
	}
}

bool PipeWireAttenuator::shouldAttenuate(const uint32_t id, const Node &node,
										 const std::vector< uint32_t > &ownSinks) const {
	if (!Global::get().s.bOnlyAttenuateSameOutput) {
		// The audio of a loopback comes from streams that we already attenuate (or from ourselves).
		return !node.loopback;
	}

	const std::vector< uint32_t > sinks = sinksOf(id);
	const bool sameOutput               = std::any_of(sinks.cbegin(), sinks.cend(), [&ownSinks](const uint32_t sink) {
		return std::find(ownSinks.cbegin(), ownSinks.cend(), sink) != ownSinks.cend();
	});
	if (!sameOutput) {
		return false;
	}

	return !node.loopback || Global::get().s.bAttenuateLoopbacks;
}

std::vector< uint32_t > PipeWireAttenuator::sinksOf(const uint32_t nodeID) const {
	std::vector< uint32_t > sinks;
	for (const auto &entry : m_links) {
		if (entry.second.outputNode == nodeID) {
			sinks.push_back(entry.second.inputNode);
		}
	}

	return sinks;
}

void PipeWireAttenuator::setVolumes(Node &node, const std::vector< float > &volumes) {
	uint8_t buffer[1024];

	spa_pod_builder builder{};
	builder.data = buffer;
	builder.size = sizeof(buffer);

	const auto param = static_cast< const spa_pod * >(
		spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_channelVolumes,
								   SPA_POD_Array(static_cast< int >(sizeof(float)), SPA_TYPE_Float,
												 static_cast< int >(volumes.size()), volumes.data())));
	if (!param) {
		return;
	}

	pw_node_set_param(reinterpret_cast< pw_node * >(node.proxy), SPA_PARAM_Props, 0, param);

	// Remember what we set, so that we can tell on restore whether somebody else changed it in the meantime.
	node.volumes = volumes;
}

void PipeWireAttenuator::onCoreDone(void *data, const uint32_t id, const int seq) {
	auto pwa = static_cast< PipeWireAttenuator * >(data);

	if (id == PW_ID_CORE && seq == pwa->m_syncSeq) {
		pwa->m_synced = true;
		pws->pw_thread_loop_signal(pwa->m_engine.m_thread, false);
	}
}

void PipeWireAttenuator::onGlobal(void *data, const uint32_t id, uint32_t, const char *type, uint32_t,
								  const spa_dict *props) {
	auto pwa = static_cast< PipeWireAttenuator * >(data);

	if (!type || !props) {
		return;
	}

	if (strcmp(type, PW_TYPE_INFO_INTERFACE_BASE "Link") == 0) {
		const auto nodeID = [props](const char *key, uint32_t &value) {
			const char *str = spa_dict_lookup(props, key);
			if (!str) {
				return false;
			}

			char *end;
			const unsigned long number = strtoul(str, &end, 10);
			if (end == str || *end != '\0' || number > std::numeric_limits< uint32_t >::max()) {
				return false;
			}

			value = static_cast< uint32_t >(number);
			return true;
		};

		Link link;
		if (nodeID(PW_KEY_LINK_OUTPUT_NODE, link.outputNode) && nodeID(PW_KEY_LINK_INPUT_NODE, link.inputNode)) {
			pwa->m_links[id]      = link;
			pwa->m_routingChanged = true;
		}

		return;
	}

	if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
		return;
	}

	const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!mediaClass || strcmp(mediaClass, "Stream/Output/Audio") != 0) {
		return;
	}

	auto proxy = static_cast< pw_proxy * >(pw_registry_bind(pwa->m_registry, id, type, PW_VERSION_NODE, 0));
	if (!proxy) {
		return;
	}

	auto node        = std::make_unique< Node >();
	node->proxy      = proxy;
	node->listener   = {};
	node->attenuated = false;
	node->loopback   = false;

	auto pwNode = reinterpret_cast< pw_node * >(proxy);
	pw_node_add_listener(pwNode, &node->listener, &pwa->m_nodeEvents, node.get());

	uint32_t params[] = { SPA_PARAM_Props };
	pw_node_subscribe_params(pwNode, params, 1);

	auto old = pwa->m_nodes.find(id);
	if (old != pwa->m_nodes.end()) {
		spa_hook_remove(&old->second->listener);
		pws->pw_proxy_destroy(old->second->proxy);
	}

	pwa->m_nodes[id] = std::move(node);
}

void PipeWireAttenuator::onGlobalRemove(void *data, const uint32_t id) {
	auto pwa = static_cast< PipeWireAttenuator * >(data);

	if (pwa->m_links.erase(id) > 0) {
		pwa->m_routingChanged = true;
	}

	auto it = pwa->m_nodes.find(id);
	if (it == pwa->m_nodes.end()) {
		return;
	}

	spa_hook_remove(&it->second->listener);
	pws->pw_proxy_destroy(it->second->proxy);
	pwa->m_nodes.erase(it);
}

void PipeWireAttenuator::onNodeInfo(void *data, const pw_node_info *info) {
	auto node = static_cast< Node * >(data);

	if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) && info->props) {
		node->loopback = spa_dict_lookup(info->props, "node.link-group") != nullptr;
	}
}

void PipeWireAttenuator::onNodeParam(void *data, int, const uint32_t id, uint32_t, uint32_t, const spa_pod *param) {
	auto node = static_cast< Node * >(data);

	if (id != SPA_PARAM_Props || !param || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props)) {
		return;
	}

	const spa_pod_prop *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes);
	if (!prop) {
		return;
	}

	float volumes[SPA_AUDIO_MAX_CHANNELS];
	const uint32_t count = spa_pod_copy_array(&prop->value, SPA_TYPE_Float, volumes, SPA_AUDIO_MAX_CHANNELS);
	if (count > 0) {
		node->volumes.assign(volumes, volumes + count);
	}
}

void PipeWireAttenuator::onTimer(void *data, uint64_t) {
	auto pwa = static_cast< PipeWireAttenuator * >(data);

	pwa->setAttenuating(pwa->m_requested.load(std::memory_order_relaxed));
}

PipeWireInput::PipeWireInput() {
	m_engine = std::make_unique< PipeWireEngine >("Capture", this, processCallback);
	if (!m_engine->isOk()) {
		return;
	}

	iMicChannels = Global::get().s.pipeWireInput;

	constexpr uint32_t CHANNELS[]{
		SPEAKER_FRONT_LEFT,
		SPEAKER_FRONT_RIGHT,
	};

	if (!m_engine->connect(PW_DIRECTION_INPUT, CHANNELS, static_cast< std::uint8_t >(iMicChannels))) {
		return;
	}

	eMicFormat = SampleFloat;
	iMicFreq   = SAMPLE_RATE;
	initializeMixer();

	m_engine->start();
}

PipeWireInput::~PipeWireInput() {
	m_engine->stop();
}

void PipeWireInput::processCallback(void *param) {
	auto pwi = static_cast< PipeWireInput * >(param);

	pw_buffer *buffer = pwi->m_engine->dequeueBuffer();
	if (!buffer) {
		return;
	}

	spa_data &data = buffer->buffer->datas[0];
	if (!data.data) {
		return;
	}

	pwi->addMic(data.data, static_cast< unsigned int >(data.chunk->size / sizeof(float)));

	pwi->m_engine->queueBuffer(buffer);
}

void PipeWireInput::run() {
}
void PipeWireInput::onUserMutedChanged() {
	if (bUserIsMuted && Global::get().s.bTxMuteCue) {
		// Do not disable the stream if mute cue is enabled (otherwise mute cue will not work).
		return;
	}

	m_engine->setActive(!bUserIsMuted);
}

PipeWireOutput::PipeWireOutput() : m_attenuationRequested(false) {
	m_engine = std::make_unique< PipeWireEngine >("Playback", this, processCallback);
	if (!m_engine->isOk()) {
		return;
	}

	iChannels = Global::get().s.pipeWireOutput;

	// This mapping works for:
	//
	// - Mono
	// - Stereo
	// - 2.1 surround
	// - 3.1 surround
	// - 5.1 surround
	// - 7.1 surround
	//
	// Ideally this should be configurable by the user.
	constexpr uint32_t CHANNELS[]{ SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_LOW_FREQUENCY, SPEAKER_FRONT_CENTER,
								   SPEAKER_BACK_LEFT,  SPEAKER_BACK_RIGHT,  SPEAKER_SIDE_LEFT,     SPEAKER_SIDE_RIGHT };

	if (!m_engine->connect(PW_DIRECTION_OUTPUT, CHANNELS, static_cast< std::uint8_t >(iChannels))) {
		return;
	}

	eSampleFormat = SampleFloat;
	iMixerFreq    = SAMPLE_RATE;
	initializeMixer(CHANNELS);

	m_attenuator = std::make_unique< PipeWireAttenuator >(*m_engine, m_attenuationRequested);

	m_engine->start();
}

PipeWireOutput::~PipeWireOutput() {
	// Needs the running thread loop to restore the volumes.
	m_attenuator.reset();

	m_engine->stop();
}

void PipeWireOutput::processCallback(void *param) {
	auto pwo = static_cast< PipeWireOutput * >(param);

	pw_buffer *buffer = pwo->m_engine->dequeueBuffer();
	if (!buffer) {
		return;
	}

	spa_data &data = buffer->buffer->datas[0];
	if (!data.data) {
		return;
	}

	spa_chunk *chunk = data.chunk;
	if (!chunk) {
		return;
	}

	chunk->offset = 0;
	chunk->stride = static_cast< int >(sizeof(float) * pwo->iChannels);

	const uint32_t frames = std::min(data.maxsize / static_cast< std::uint32_t >(chunk->stride), pwo->iFrameSize);

	chunk->size = frames * static_cast< unsigned int >(chunk->stride);

	const bool mixed = pwo->mix(data.data, frames);
	pwo->m_attenuationRequested.store(AudioOutput::shouldAttenuateOthers(mixed), std::memory_order_relaxed);
	if (!mixed) {
		// When the mixer has no data available to write, we still need to push silence.
		// This is to avoid an infinite loop when destroying the stream.
		// In that infinite loop, Pipewire would wait until the stream starts draining.
		// But this never happens, if we don't push new data.
		// Thus pw_stream_destroy() would block forever.
		memset(data.data, 0, sizeof(float) * chunk->size);
	}

	pwo->m_engine->queueBuffer(buffer);
}

void PipeWireOutput::run() {
}

#undef RESOLVE
