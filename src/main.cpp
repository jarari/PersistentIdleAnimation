#include "Utilities.h"
#include <detourxs/detourxs.h>
#include <fstream>
#include <thread>
#include <chrono>
//#define DEBUG

#ifdef DEBUG
#define _DEBUGMESSAGE(fmt, ...) _MESSAGE(fmt __VA_OPT__(, ) __VA_ARGS__)
#else
#define _DEBUGMESSAGE(fmt, ...)
#endif

using namespace RE;
using std::unordered_map;

AnimationFileManagerSingleton* afm;
PlayerCharacter* p;
PlayerCamera* pc;
PlayerControls* pcon;
REL::Relocation<uintptr_t> ptr_AnimationFileManagerUpdate{ REL::ID(556439), 0x40 };
uintptr_t AnimationFileManagerUpdateOrig;
REL::Relocation<BSTArray<LoadedIdleAnimData>*> LoadedHandleAndBindingA{ REL::ID(762973) };
REL::Relocation<BSTArray<LoadingIdleAnimData>*> LoadingQueue{ REL::ID(57925) };
REL::Relocation<BSReadWriteLock*> LoadedIdleLock{ REL::ID(1420624) };
static unordered_map<std::string, std::string> persistentIdleList;
static unordered_map<std::string, std::string>::iterator cacheIterator;
static unordered_map<std::string, std::string>::iterator cacheCurrentIterator;
static unordered_map<std::string, uint32_t> cacheIndexMap;
static bool hasManager = false;
static bool hasGraph = false;
static bool cached = false;
static int cacheCount = 0;
static int targetCount = 0;
static int cachingStatus = 0;
static bool isLoading = false;
static bool wasDrawn = false;
static bool updateAnimGraph = false;
static float targetScale = -1.f;
static float lastCacheRequest = 0.f;
static float loadedTime = std::numeric_limits<float>::infinity();
static NiAVObject* fakeSkeleton = nullptr;
static TESCameraState* lastState = nullptr;
static hkbClipGenerator* dynamicIdleClip = nullptr;

void CheckCachingCompletion() {
	if (cacheCount == targetCount) {
		_MESSAGE("Cached %d animations", cacheCount);
		pc->SetState(lastState);
		std::thread([]() {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			F4SE::GetTaskInterface()->AddTask([]() {
				p->NotifyAnimationGraphImpl("IdleStop");
			});
		}).detach();

		LoadedIdleLock->lock_read();
		int index = 0;
		for (auto it = LoadedHandleAndBindingA->begin(); it != LoadedHandleAndBindingA->end(); ++it) {
			if (persistentIdleList.find((*it).path.c_str()) != persistentIdleList.end()) {
				_DEBUGMESSAGE("cacheIndexMap %s %d", (*it).path.c_str(), index);
				cacheIndexMap.insert(std::pair<std::string, uint32_t>((*it).path.c_str(), index));
			}
			++index;
		}
		LoadedIdleLock->unlock_read();
		if (p->Get3D(false)) {
			p->Get3D(false)->local.scale = targetScale;
		}
		cached = true;
	}
}

class AnimationFileManagerSingletonOverride {
public:
	typedef void (AnimationFileManagerSingletonOverride::* FnDeactivateDynamicClipImpl)(hkbContext&, hkbClipGenerator&);

	void HookedDeactivateDynamicClipImpl(hkbContext& context, hkbClipGenerator& clip) {
		bool processOrig = true;
		std::string path = *(const char**)((uintptr_t)&clip + 0x90) - 0x1;
		if (persistentIdleList.find(path) != persistentIdleList.end()) {
			int loadedCount = 0;
			for (auto it = LoadedHandleAndBindingA->begin(); it != LoadedHandleAndBindingA->end(); ++it) {
				if ((*it).path == path) {
					++loadedCount;
				}
			}
			if (loadedCount <= 1) {
				processOrig = false;
				_DEBUGMESSAGE("Prevented unload of %s", path.c_str());
			}
		}
		if (cachingStatus >= 1 && path == cacheCurrentIterator->first) {
			cachingStatus = 0;
			++cacheCount;
			++cacheIterator;
			if (p->Get3D(false)) {
				p->Get3D(false)->local.scale = max((float)cacheCount / (float)targetCount * targetScale, 0.01f);
			}
			_DEBUGMESSAGE("cacheCount %d", cacheCount);
			CheckCachingCompletion();
		}
		FnDeactivateDynamicClipImpl fn = fnHash.at(*(uintptr_t*)this);
		if (fn && processOrig) {
			(this->*fn)(context, clip);
		}
	}

	static void HookDeactivate(uintptr_t vtable) {
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnDeactivateDynamicClipImpl fn = SafeWrite64Function(vtable + 0x18, &AnimationFileManagerSingletonOverride::HookedDeactivateDynamicClipImpl);
			fnHash.insert(std::pair<uintptr_t, FnDeactivateDynamicClipImpl>(vtable, fn));
		}
	}

protected:
	static unordered_map<uintptr_t, FnDeactivateDynamicClipImpl> fnHash;
};
unordered_map<uintptr_t, AnimationFileManagerSingletonOverride::FnDeactivateDynamicClipImpl> AnimationFileManagerSingletonOverride::fnHash;

class hkbClipGeneratorWatcher {
public:
	typedef void (hkbClipGeneratorWatcher::* FnActivate)(hkbContext&);
	typedef void (hkbClipGeneratorWatcher::* FnUpdate)(hkbContext&, float);

	void HookedActivate(hkbContext& context) {
		FnActivate fn = fnHash.at(*(uintptr_t*)this);
		if (fn) {
			(this->*fn)(context);
		}
	}

	void HookedUpdate(hkbContext& context, float deltaTime) {
		bool processOrig = true;
		if (!cached && cachingStatus >= 1) {
			std::string path = *(const char**)((uintptr_t)this + 0x90) - 0x1;
			//_DEBUGMESSAGE("path %s", path.c_str());
			if (path == cacheCurrentIterator->first) {
				processOrig = false;
				FnUpdate fn = fnHash2.at(*(uintptr_t*)this);
				if (fn) {
					(this->*fn)(context, 0.f);
				}
				//_DEBUGMESSAGE("Match found");
				if (cachingStatus == 1) {
					cachingStatus = 2;
					LoadedIdleLock->lock_read();
					for (auto it = LoadedHandleAndBindingA->begin(); it != LoadedHandleAndBindingA->end(); ++it) {
						if ((*it).path == path && (*it).bindingWithTriggers) {
							F4SE::GetTaskInterface()->AddTask([]() {
								p->UpdateAnimation(1000.f);
								p->NotifyAnimationGraphImpl("IdleStop");
								//_DEBUGMESSAGE("Skip");
							});
							break;
						}
					}
					LoadedIdleLock->unlock_read();
				}
				else {
					//_DEBUGMESSAGE("Loop prevention");
					cachingStatus = 1;
				}
				//_MESSAGE("clip %s path %s", *(const char**)((uintptr_t)this + 0x38), path.c_str());
			}
		}

		if (processOrig) {
			FnUpdate fn = fnHash2.at(*(uintptr_t*)this);
			if (fn) {
				(this->*fn)(context, deltaTime);
			}
		}
	}

	static void HookFunctions(uintptr_t vtable) {
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnActivate fn = SafeWrite64Function(vtable + 0x38, &hkbClipGeneratorWatcher::HookedActivate);
			fnHash.insert(std::pair<uintptr_t, FnActivate>(vtable, fn));
		}
		auto it2 = fnHash2.find(vtable);
		if (it2 == fnHash2.end()) {
			FnUpdate fn2 = SafeWrite64Function(vtable + 0x40, &hkbClipGeneratorWatcher::HookedUpdate);
			fnHash2.insert(std::pair<uintptr_t, FnUpdate>(vtable, fn2));
		}
	}

protected:
	static unordered_map<uintptr_t, FnActivate> fnHash;
	static unordered_map<uintptr_t, FnUpdate> fnHash2;
};
unordered_map<uintptr_t, hkbClipGeneratorWatcher::FnActivate> hkbClipGeneratorWatcher::fnHash;
unordered_map<uintptr_t, hkbClipGeneratorWatcher::FnUpdate> hkbClipGeneratorWatcher::fnHash2;

class ConstructAnimationGraphWatcher {
public:
	typedef bool (ConstructAnimationGraphWatcher::* FnSetAnimationGraphManagerImpl)(BSTSmartPointer<BSAnimationGraphManager> const&);
	typedef bool (ConstructAnimationGraphWatcher::* FnConstructAnimationGraph)(BSTSmartPointer<BShkbAnimationGraph> const&);

	bool HookedSetAnimationGraphManagerImpl(BSTSmartPointer<BSAnimationGraphManager> const& animGraphManager) {
		FnSetAnimationGraphManagerImpl fn = fnHash.at(*(uintptr_t*)this);
		if (!cached) {
			if (fn && (this->*fn)(animGraphManager)) {
				_MESSAGE("animGraphManager set");
				hasManager = true;
				return true;
			}
		}
		else {
			/*Actor* a = (Actor*)((uintptr_t)this - 0x48);
			if (p == a) {
				updateAnimGraph = true;
			}*/
			return fn ? (this->*fn)(animGraphManager) : false;
		}
		return false;
	}

	bool HookedConstructAnimationGraph(BSTSmartPointer<BShkbAnimationGraph> const& animGraph) {
		FnConstructAnimationGraph fn = fnHash2.at(*(uintptr_t*)this);
		if (!cached) {
			if (fn && (this->*fn)(animGraph)) {
				_MESSAGE("animGraph set");
				hasGraph = true;
				return true;
			}
		}
		else {
			return fn ? (this->*fn)(animGraph) : false;
		}
		return false;
	}

	static void HookFunctions(uintptr_t vtable) {
		auto it = fnHash.find(vtable);
		if (it == fnHash.end()) {
			FnSetAnimationGraphManagerImpl fn = SafeWrite64Function(vtable + 0x28, &ConstructAnimationGraphWatcher::HookedSetAnimationGraphManagerImpl);
			fnHash.insert(std::pair<uintptr_t, FnSetAnimationGraphManagerImpl>(vtable, fn));
		}
		auto it2 = fnHash2.find(vtable);
		if (it2 == fnHash2.end()) {
			FnConstructAnimationGraph fn2 = SafeWrite64Function(vtable + 0x38, &ConstructAnimationGraphWatcher::HookedConstructAnimationGraph);
			fnHash2.insert(std::pair<uintptr_t, FnConstructAnimationGraph>(vtable, fn2));
		}
	}

protected:
	static unordered_map<uintptr_t, FnSetAnimationGraphManagerImpl> fnHash;
	static unordered_map<uintptr_t, FnConstructAnimationGraph> fnHash2;
};
unordered_map<uintptr_t, ConstructAnimationGraphWatcher::FnSetAnimationGraphManagerImpl> ConstructAnimationGraphWatcher::fnHash;
unordered_map<uintptr_t, ConstructAnimationGraphWatcher::FnConstructAnimationGraph> ConstructAnimationGraphWatcher::fnHash2;

class MenuWatcher {
	virtual ~MenuWatcher() {};
	virtual BSEventNotifyControl ProcessEvent(const MenuOpenCloseEvent& evn, BSTEventSource<MenuOpenCloseEvent>* src) {
		if (evn.menuName == BSFixedString("LoadingMenu")) {
			if (evn.opening) {
				isLoading = true;
			}
			else {
				loadedTime = *F4::ptr_engineTime;
				wasDrawn = p->weaponState >= WEAPON_STATE::kWantToDraw && p->weaponState <= WEAPON_STATE::kDrawn;
				isLoading = false;
			}
		}
		if (cached) {
			isLoading = false;
			UI::GetSingleton()->GetEventSource<MenuOpenCloseEvent>()->UnregisterSink((BSTEventSink<MenuOpenCloseEvent>*)this);
			delete(this);
		}
		return BSEventNotifyControl::kContinue;
	}
};

void HookedAnimationFileManagerUpdate(AnimationFileManagerSingleton* filemanager) {
	if (!cached && hasManager && hasGraph && *F4::ptr_engineTime - loadedTime >= 2.f && p->interactingState == INTERACTING_STATE::kNotInteracting) {
		if (!lastState)
			lastState = pc->currentState.get();
		if (targetScale < 0 && p->Get3D(false)) {
			targetScale = p->Get3D(false)->local.scale;
			p->Get3D(false)->local.scale = max((float)cacheCount / (float)targetCount * targetScale, 0.01f);
		}
		if (pc->currentState != pc->cameraStates[CameraStates::k3rdPerson]) {
			pc->Force3rdPerson();
		}
		else {
			BSTSmartPointer<BSAnimationGraphManager>& animGraphManager = p->currentProcess->middleHigh->animationGraphManager;
			if (animGraphManager.get()) {
				BSTSmartPointer<BShkbAnimationGraph>& animGraph = *(((BSTSmartPointer<BShkbAnimationGraph>**)((uintptr_t)animGraphManager.get() + 0x48))[0]);
				if (cacheIterator != persistentIdleList.end()) {
					if (!afm->IsLoading(cacheIterator->first, animGraphManager) && cachingStatus == 0) {
						if (!isLoading) {
							if (cacheCurrentIterator != cacheIterator) {
								_MESSAGE("Caching %s", cacheIterator->first.c_str());
								cacheCurrentIterator = cacheIterator;
								lastCacheRequest = *F4::ptr_engineTime;
								bool succ = afm->RequestIdles(cacheIterator->second, animGraph, cacheIterator->first, animGraphManager);
								if (!succ) {
									_MESSAGE("Failed to cache %s", cacheIterator->first.c_str());
									--targetCount;
									++cacheIterator;
									CheckCachingCompletion();
								}
								else {
									cachingStatus = 1;
								}
							}
						}
					}
					else if (*F4::ptr_engineTime - lastCacheRequest >= 1.f) {
						_MESSAGE("Cache retry for %s", cacheCurrentIterator->first.c_str());
						p->UpdateAnimation(1000.f);
						p->NotifyAnimationGraphImpl("IdleStop");
						bool succ = afm->RequestIdles(cacheCurrentIterator->second, animGraph, cacheCurrentIterator->first, animGraphManager);
						if (!succ) {
							_MESSAGE("Failed to cache %s", cacheCurrentIterator->first.c_str());
							--targetCount;
							++cacheIterator;
							cachingStatus = 0;
							CheckCachingCompletion();
						}
					}
				}
			}
		}
	}
	/*else if (updateAnimGraph && p->currentProcess && p->currentProcess->middleHigh) {
		_MESSAGE("PC animGraph update");
		BSTSmartPointer<BSAnimationGraphManager>& animGraphManager = p->currentProcess->middleHigh->animationGraphManager;
		if (animGraphManager.get()) {
			BSTSmartPointer<BShkbAnimationGraph>& animGraph = *(((BSTSmartPointer<BShkbAnimationGraph>**)((uintptr_t)animGraphManager.get() + 0x48))[0]);
			LoadedIdleLock->lock_write();
			for (auto it = LoadedHandleAndBindingA->begin(); it != LoadedHandleAndBindingA->end(); ++it) {
				if (persistentIdleList.find((*it).path.c_str()) != persistentIdleList.end()) {
					_MESSAGE("%s animGraph reset", (*it).path.c_str());
					(*it).hkbAnimGraph = animGraph.get();
				}
			}
			LoadedIdleLock->unlock_write();
			updateAnimGraph = false;
		}
	}*/
	LoadedIdleLock->lock_write();
	for (auto it = LoadingQueue->begin(); it != LoadingQueue->end(); ++it) {
		if (persistentIdleList.find((*it).path.c_str()) != persistentIdleList.end()) {
			for (auto it2 = LoadedHandleAndBindingA->begin(); it2 != LoadedHandleAndBindingA->end(); ++it2) {
				if ((*it).resourceHandle == (*it2).resourceHandle && (*it2).bindingWithTriggers) {
					(*it2).hkbAnimGraph = (*it).hkbAnimGraph;
					(*it2).clipGenerator = (void*)0xFFFFFFFF;
					(*it2).bindingWithTriggers = nullptr;
					_DEBUGMESSAGE("Cache found");
					break;
				}
			}
		}
	}
	LoadedIdleLock->unlock_write();
	using FnUpdate = decltype(&HookedAnimationFileManagerUpdate);
	FnUpdate fn = (FnUpdate)AnimationFileManagerUpdateOrig;
	if (fn)
		(*fn)(filemanager);
}

void InitializePlugin() {
	p = PlayerCharacter::GetSingleton();
	pc = PlayerCamera::GetSingleton();
	pcon = PlayerControls::GetSingleton();
	afm = AnimationFileManagerSingleton::GetSingleton();

	AnimationFileManagerSingletonOverride::HookDeactivate(REL::Relocation<uintptr_t>{ VTABLE::AnimationFileManagerSingleton[0] }.address());
	hkbClipGeneratorWatcher::HookFunctions(REL::Relocation<uintptr_t>{ VTABLE::hkbClipGenerator[0] }.address());
	ConstructAnimationGraphWatcher::HookFunctions(REL::Relocation<uintptr_t>{ VTABLE::PlayerCharacter[5] }.address());
	MenuWatcher* mw = new MenuWatcher();
	UI::GetSingleton()->GetEventSource<MenuOpenCloseEvent>()->RegisterSink((BSTEventSink<MenuOpenCloseEvent>*)mw);

	namespace fs = std::filesystem;
	fs::path dataPath = fs::current_path();
	dataPath += "\\Data\\F4SE\\Plugins\\PIA";
	std::stringstream stream;
	fs::directory_entry dataEntry{ dataPath };
	if (!dataEntry.exists()) {
		_MESSAGE("PIA directory does not exist!");
		return;
	}
	for (auto& it : fs::directory_iterator(dataEntry)) {
		if (it.path().extension().compare(".txt") == 0) {
			stream << it.path().filename();
			_MESSAGE("Loading animation data %s", stream.str().c_str());
			stream.str(std::string());
			std::ifstream reader(it.path());
			if (reader.is_open()) {
				std::string line;
				while (std::getline(reader, line)) {
					std::string formIDstr;
					std::string plugin = SplitString(line, "|", formIDstr);
					if (formIDstr.length() != 0) {
						uint32_t formID = std::stoi(formIDstr, 0, 16);
						TESIdleForm* idle = (TESIdleForm*)GetFormFromMod(plugin, formID);
						if (idle && idle->formType == ENUM_FORM_ID::kIDLE) {
							std::string path = idle->animFileName.c_str();
							if (idle->animEventName.contains("dyn_")) {
								auto succ = persistentIdleList.insert(std::pair<std::string, std::string>(path, idle->animEventName.c_str()));
								if (succ.second) {
									++targetCount;
									_MESSAGE("Added %s", path.c_str());
								}
							}
							else {
								_MESSAGE("Skipped %s (Not a dynamic idle animation)", path.c_str());
							}
						}
					}
				}
				reader.close();
			}
		}
	}
	cacheIterator = persistentIdleList.begin();
	cacheCurrentIterator = persistentIdleList.end();
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface * a_f4se, F4SE::PluginInfo * a_info) {
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::warn);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("%g(%#): [%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical(FMT_STRING("loaded in editor"));
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	F4SE::AllocTrampoline(8 * 8);

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface * a_f4se) {
	F4SE::Init(a_f4se);

	F4SE::Trampoline& trampoline = F4SE::GetTrampoline();
	AnimationFileManagerUpdateOrig = trampoline.write_call<5>(ptr_AnimationFileManagerUpdate.address(), &HookedAnimationFileManagerUpdate);

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kGameDataReady) {
			InitializePlugin();
		}
	});

	return true;
}
