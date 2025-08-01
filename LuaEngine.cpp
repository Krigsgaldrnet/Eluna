/*
* Copyright (C) 2010 - 2024 Eluna Lua Engine <https://elunaluaengine.github.io/>
* This program is free software licensed under GPL version 3
* Please see the included DOCS/LICENSE.md for more information
*/

#include "Hooks.h"
#include "LuaEngine.h"
#include "BindingMap.h"
#include "ElunaCompat.h"
#include "ElunaConfig.h"
#include "ElunaEventMgr.h"
#include "ElunaIncludes.h"
#include "ElunaLoader.h"
#include "ElunaTemplate.h"
#include "ElunaUtility.h"
#include "ElunaCreatureAI.h"
#include "ElunaInstanceAI.h"

extern "C"
{
// Base lua libraries
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

// Additional lua libraries
};

extern void RegisterMethods(Eluna* E);

void Eluna::_ReloadEluna()
{
    // Remove all timed events
    eventMgr->SetStates(LUAEVENT_STATE_ERASE);

#if defined ELUNA_TRINITY
    // Cancel all pending async queries
    GetQueryProcessor().CancelAll();
#endif

    // Close lua
    CloseLua();

    // Open new lua and libraries
    OpenLua();

    // Run scripts from loaded paths
    RunScripts();

    reload = false;
}

Eluna::Eluna(Map* map) :
event_level(0),
push_counter(0),
boundMap(map),
L(NULL)
{
    OpenLua();
    eventMgr = std::make_unique<EventMgr>(this);

    // if the script cache is ready, run scripts, otherwise flag state for reload
    if (sElunaLoader->GetCacheState() == SCRIPT_CACHE_READY)
        RunScripts();
    else
        reload = true;
}

Eluna::~Eluna()
{
    CloseLua();
}

void Eluna::CloseLua()
{
    OnLuaStateClose();

    DestroyBindStores();

    // Must close lua state after deleting stores and mgr
    if (L)
        lua_close(L);
    L = NULL;

    instanceDataRefs.clear();
    continentDataRefs.clear();
}

static int PrecompiledLoader(lua_State* L)
{
    const char* modname = lua_tostring(L, 1);
    if (modname == NULL)
        return 0;

    const std::vector<LuaScript>& scripts = sElunaLoader->GetLuaScripts();

    auto it = std::find_if(scripts.begin(), scripts.end(), [modname](const LuaScript& script) { return script.filename == modname; });
    if (it == scripts.end()) {
        lua_pushfstring(L, "\n\tno precompiled script '%s' found", modname);
        return 1;
    }
    if (luaL_loadbuffer(L, reinterpret_cast<const char*>(&it->bytecode[0]), it->bytecode.size(), it->filename.c_str()))
    {
        // Stack: modname, errmsg
        return lua_error(L);
    }
    // Stack: modname, filefunction
    lua_pushstring(L, it->filepath.c_str());
    // Stack: modname, filefunction, modpath
    return 2;
}

void Eluna::OpenLua()
{
    L = luaL_newstate();

    lua_pushlightuserdata(L, this);
    lua_setfield(L, LUA_REGISTRYINDEX, ELUNA_STATE_PTR);

    CreateBindStores();

    // open base lua libraries
    luaL_openlibs(L);

    // Register methods and functions
    RegisterMethods(this);

    // get require paths
    const std::string& requirepath = sElunaLoader->GetRequirePath();
    const std::string& requirecpath = sElunaLoader->GetRequireCPath();

    // Set lua require folder paths (scripts folder structure)
    lua_getglobal(L, "package");
    lua_pushstring(L, requirepath.c_str());
    lua_setfield(L, -2, "path");
    lua_pushstring(L, requirecpath.c_str());
    lua_setfield(L, -2, "cpath");
    // Set package.loaders loader for precompiled scripts
    lua_getfield(L, -1, "loaders");
    if (lua_isnil(L, -1)) {
        // Lua 5.2+ uses searchers instead of loaders
        lua_pop(L, 1);
        lua_getfield(L, -1, "searchers");
    }
    // insert the new loader to the loaders table by shifting other elements down by one
    const int newLoaderIndex = 1;
    for (int i = lua_rawlen(L, -1); i >= newLoaderIndex; --i) {
        lua_rawgeti(L, -1, i);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushcfunction(L, &PrecompiledLoader);
    lua_rawseti(L, -2, newLoaderIndex);
    lua_pop(L, 2); // pop loaders/searchers table, pop package table
}

void Eluna::CreateBindStores()
{
    DestroyBindStores();

    CreateBinding<EventKey<Hooks::ServerEvents>>(Hooks::REGTYPE_SERVER);
    CreateBinding<EventKey<Hooks::PlayerEvents>>(Hooks::REGTYPE_PLAYER);
    CreateBinding<EventKey<Hooks::GuildEvents>>(Hooks::REGTYPE_GUILD);
    CreateBinding<EventKey<Hooks::GroupEvents>>(Hooks::REGTYPE_GROUP);
    CreateBinding<EventKey<Hooks::VehicleEvents>>(Hooks::REGTYPE_VEHICLE);
    CreateBinding<EventKey<Hooks::BGEvents>>(Hooks::REGTYPE_BG);

    CreateBinding<EntryKey<Hooks::PacketEvents>>(Hooks::REGTYPE_PACKET);
    CreateBinding<EntryKey<Hooks::CreatureEvents>>(Hooks::REGTYPE_CREATURE);
    CreateBinding<EntryKey<Hooks::GossipEvents>>(Hooks::REGTYPE_CREATURE_GOSSIP);
    CreateBinding<EntryKey<Hooks::GameObjectEvents>>(Hooks::REGTYPE_GAMEOBJECT);
    CreateBinding<EntryKey<Hooks::GossipEvents>>(Hooks::REGTYPE_GAMEOBJECT_GOSSIP);
    CreateBinding<EntryKey<Hooks::SpellEvents>>(Hooks::REGTYPE_SPELL);
    CreateBinding<EntryKey<Hooks::ItemEvents>>(Hooks::REGTYPE_ITEM);
    CreateBinding<EntryKey<Hooks::GossipEvents>>(Hooks::REGTYPE_ITEM_GOSSIP);
    CreateBinding<EntryKey<Hooks::GossipEvents>>(Hooks::REGTYPE_PLAYER_GOSSIP);
    CreateBinding<EntryKey<Hooks::InstanceEvents>>(Hooks::REGTYPE_MAP);
    CreateBinding<EntryKey<Hooks::InstanceEvents>>(Hooks::REGTYPE_INSTANCE);

    CreateBinding<UniqueObjectKey<Hooks::CreatureEvents>>(Hooks::REGTYPE_CREATURE_UNIQUE);
}

void Eluna::DestroyBindStores()
{
    for (auto& binding : bindingMaps)
        binding.reset();
}

void Eluna::RunScripts()
{
    int32 const boundMapId = GetBoundMapId();
    uint32 const boundInstanceId = GetBoundInstanceId();
    ELUNA_LOG_DEBUG("[Eluna]: Running scripts for state: %i, instance: %u", boundMapId, boundInstanceId);

    uint32 oldMSTime = ElunaUtil::GetCurrTime();
    uint32 count = 0;

    std::unordered_map<std::string, std::string> loaded; // filename, path

    lua_getglobal(L, "require");
    // Stack: require

    const std::vector<LuaScript>& scripts = sElunaLoader->GetLuaScripts();

    for (auto it = scripts.begin(); it != scripts.end(); ++it)
    {
        // check that the script file is either global or meant to be loaded for this map
        if (it->mapId != -1 && it->mapId != boundMapId)
        {
            ELUNA_LOG_DEBUG("[Eluna]: `%s` is tagged %i and will not load for map: %i", it->filename.c_str(), it->mapId, boundMapId);
            continue;
        }

        // Check that no duplicate names exist
        if (loaded.find(it->filename) != loaded.end())
        {
            ELUNA_LOG_ERROR("[Eluna]: Error loading `%s`. File with same name already loaded from `%s`, rename either file", it->filepath.c_str(), loaded[it->filename].c_str());
            continue;
        }
        loaded[it->filename] = it->filepath;

        // We call require on the filename to load the script
        // A custom loader is used to load the script from the combined_scripts table
        // The loader is set up in Eluna::OpenLua
        lua_pushvalue(L, -1); // Stack: require, require
        lua_pushstring(L, it->filename.c_str()); // Stack: require, require, filename
        if (ExecuteCall(1, 0))
        {
            // Successfully called require on the script
            ELUNA_LOG_DEBUG("[Eluna]: Successfully loaded `%s`", it->filepath.c_str());
            ++count;
            continue;
        }
        // Stack: require
    }
    // Stack: require
    lua_pop(L, 1);
    ELUNA_LOG_INFO("[Eluna]: Executed %u Lua scripts in %u ms for map: %i, instance: %u", count, ElunaUtil::GetTimeDiff(oldMSTime), boundMapId, boundInstanceId);

    OnLuaStateOpen();
}

#if !defined TRACKABLE_PTR_NAMESPACE
void Eluna::InvalidateObjects()
{
    ++callstackid;
    ASSERT(callstackid && "Callstackid overflow");
}
#endif

void Eluna::Report(lua_State* _L)
{
    const char* msg = lua_tostring(_L, -1);
    ELUNA_LOG_ERROR("%s", msg);
    lua_pop(_L, 1);
}

// Borrowed from http://stackoverflow.com/questions/12256455/print-stacktrace-from-c-code-with-embedded-lua
int Eluna::StackTrace(lua_State* _L)
{
    // Stack: errmsg
    if (!lua_isstring(_L, -1))  /* 'message' not a string? */
        return 1;  /* keep it intact */
    // Stack: errmsg, debug
    lua_getglobal(_L, "debug");
    if (!lua_istable(_L, -1))
    {
        lua_pop(_L, 1);
        return 1;
    }
    // Stack: errmsg, debug, traceback
    lua_getfield(_L, -1, "traceback");
    if (!lua_isfunction(_L, -1))
    {
        lua_pop(_L, 2);
        return 1;
    }
    lua_pushvalue(_L, -3);  /* pass error message */
    lua_pushinteger(_L, 1);  /* skip this function and traceback */
    // Stack: errmsg, debug, traceback, errmsg, 2
    lua_call(_L, 2, 1);  /* call debug.traceback */

    // dirty stack?
    // Stack: errmsg, debug, tracemsg
    return 1;
}

bool Eluna::ExecuteCall(int params, int res)
{
    int top = lua_gettop(L);
    int base = top - params;

    // Expected: function, [parameters]
    ASSERT(base > 0);

    // Check function type
    if (!lua_isfunction(L, base))
    {
        ELUNA_LOG_ERROR("[Eluna]: Cannot execute call: registered value is %s, not a function.", luaL_tolstring(L, base, NULL));
        ASSERT(false); // stack probably corrupt
    }

    bool usetrace = sElunaConfig->GetConfig(CONFIG_ELUNA_TRACEBACK);
    if (usetrace)
    {
        lua_pushcfunction(L, &StackTrace);
        // Stack: function, [parameters], traceback
        lua_insert(L, base);
        // Stack: traceback, function, [parameters]
    }

    // Objects are invalidated when event_level hits 0
    ++event_level;
    int result = lua_pcall(L, params, res, usetrace ? base : 0);
    --event_level;

    if (usetrace)
    {
        // Stack: traceback, [results or errmsg]
        lua_remove(L, base);
    }
    // Stack: [results or errmsg]

    // lua_pcall returns 0 on success.
    // On error print the error and push nils for expected amount of returned values
    if (result)
    {
        // Stack: errmsg
        Report(L);

        // Force garbage collect
        lua_gc(L, LUA_GCCOLLECT, 0);

        // Push nils for expected amount of results
        for (int i = 0; i < res; ++i)
            lua_pushnil(L);
        // Stack: [nils]
        return false;
    }

    // Stack: [results]
    return true;
}

void Eluna::Push()
{
    lua_pushnil(L);
}
void Eluna::Push(const long long l)
{
    // pushing pointer to local is fine, a copy of value will be stored, not pointer itself
    ElunaTemplate<long long>::Push(this, &l);
}
void Eluna::Push(const unsigned long long l)
{
    // pushing pointer to local is fine, a copy of value will be stored, not pointer itself
    ElunaTemplate<unsigned long long>::Push(this, &l);
}
void Eluna::Push(const long l)
{
    Push(static_cast<long long>(l));
}
void Eluna::Push(const unsigned long l)
{
    Push(static_cast<unsigned long long>(l));
}
void Eluna::Push(const int i)
{
    lua_pushinteger(L, i);
}
void Eluna::Push(const unsigned int u)
{
    lua_pushunsigned(L, u);
}
void Eluna::Push(const double d)
{
    lua_pushnumber(L, d);
}
void Eluna::Push(const float f)
{
    lua_pushnumber(L, f);
}
void Eluna::Push(const bool b)
{
    lua_pushboolean(L, b);
}
void Eluna::Push(const std::string& str)
{
    lua_pushstring(L, str.c_str());
}
void Eluna::Push(const char* str)
{
    lua_pushstring(L, str);
}
void Eluna::Push(Pet const* pet)
{
    Push<Creature>(pet);
}
void Eluna::Push(TempSummon const* summon)
{
    Push<Creature>(summon);
}
void Eluna::Push(Unit const* unit)
{
    if (!unit)
    {
        Push();
        return;
    }
    switch (unit->GetTypeId())
    {
        case TYPEID_UNIT:
            Push(unit->ToCreature());
            break;
        case TYPEID_PLAYER:
            Push(unit->ToPlayer());
            break;
        default:
            ElunaTemplate<Unit>::Push(this, unit);
    }
}
void Eluna::Push(WorldObject const* obj)
{
    if (!obj)
    {
        Push();
        return;
    }
    switch (obj->GetTypeId())
    {
        case TYPEID_UNIT:
            Push(obj->ToCreature());
            break;
        case TYPEID_PLAYER:
            Push(obj->ToPlayer());
            break;
        case TYPEID_GAMEOBJECT:
            Push(obj->ToGameObject());
            break;
        case TYPEID_CORPSE:
            Push(obj->ToCorpse());
            break;
        default:
            ElunaTemplate<WorldObject>::Push(this, obj);
    }
}
void Eluna::Push(Object const* obj)
{
    if (!obj)
    {
        Push();
        return;
    }
    switch (obj->GetTypeId())
    {
        case TYPEID_UNIT:
            Push(obj->ToCreature());
            break;
        case TYPEID_PLAYER:
            Push(obj->ToPlayer());
            break;
        case TYPEID_GAMEOBJECT:
            Push(obj->ToGameObject());
            break;
        case TYPEID_CORPSE:
            Push(obj->ToCorpse());
            break;
        default:
            ElunaTemplate<Object>::Push(this, obj);
    }
}
void Eluna::Push(ObjectGuid const guid)
{
    // pushing pointer to local is fine, a copy of value will be stored, not pointer itself
    ElunaTemplate<ObjectGuid>::Push(this, &guid);
}

static int CheckIntegerRange(lua_State* luastate, int narg, int min, int max)
{
    double value = luaL_checknumber(luastate, narg);
    char error_buffer[64];

    if (value > max)
    {
        snprintf(error_buffer, 64, "value must be less than or equal to %i", max);
        return luaL_argerror(luastate, narg, error_buffer);
    }

    if (value < min)
    {
        snprintf(error_buffer, 64, "value must be greater than or equal to %i", min);
        return luaL_argerror(luastate, narg, error_buffer);
    }

    return static_cast<int>(value);
}

static unsigned int CheckUnsignedRange(lua_State* luastate, int narg, unsigned int max)
{
    double value = luaL_checknumber(luastate, narg);

    if (value < 0)
        return luaL_argerror(luastate, narg, "value must be greater than or equal to 0");

    if (value > max)
    {
        char error_buffer[64];
        snprintf(error_buffer, 64, "value must be less than or equal to %u", max);
        return luaL_argerror(luastate, narg, error_buffer);
    }

    return static_cast<unsigned int>(value);
}

template<> bool Eluna::CHECKVAL<bool>(int narg)
{
    return lua_toboolean(L, narg) != 0;
}
template<> float Eluna::CHECKVAL<float>(int narg)
{
    return static_cast<float>(luaL_checknumber(L, narg));
}
template<> double Eluna::CHECKVAL<double>(int narg)
{
    return luaL_checknumber(L, narg);
}
template<> signed char Eluna::CHECKVAL<signed char>(int narg)
{
    return CheckIntegerRange(L, narg, SCHAR_MIN, SCHAR_MAX);
}
template<> unsigned char Eluna::CHECKVAL<unsigned char>(int narg)
{
    return CheckUnsignedRange(L, narg, UCHAR_MAX);
}
template<> short Eluna::CHECKVAL<short>(int narg)
{
    return CheckIntegerRange(L, narg, SHRT_MIN, SHRT_MAX);
}
template<> unsigned short Eluna::CHECKVAL<unsigned short>(int narg)
{
    return CheckUnsignedRange(L, narg, USHRT_MAX);
}
template<> int Eluna::CHECKVAL<int>(int narg)
{
    return CheckIntegerRange(L, narg, INT_MIN, INT_MAX);
}
template<> unsigned int Eluna::CHECKVAL<unsigned int>(int narg)
{
    return CheckUnsignedRange(L, narg, UINT_MAX);
}
template<> const char* Eluna::CHECKVAL<const char*>(int narg)
{
    return luaL_checkstring(L, narg);
}
template<> std::string Eluna::CHECKVAL<std::string>(int narg)
{
    return luaL_checkstring(L, narg);
}
template<> long long Eluna::CHECKVAL<long long>(int narg)
{
    if (lua_isnumber(L, narg))
        return static_cast<long long>(CHECKVAL<double>(narg));
    return *(Eluna::CHECKOBJ<long long>(narg, true));
}
template<> unsigned long long Eluna::CHECKVAL<unsigned long long>(int narg)
{
    if (lua_isnumber(L, narg))
        return static_cast<unsigned long long>(CHECKVAL<uint32>(narg));
    return *(Eluna::CHECKOBJ<unsigned long long>(narg, true));
}
template<> long Eluna::CHECKVAL<long>(int narg)
{
    return static_cast<long>(CHECKVAL<long long>(narg));
}
template<> unsigned long Eluna::CHECKVAL<unsigned long>(int narg)
{
    return static_cast<unsigned long>(CHECKVAL<unsigned long long>(narg));
}
template<> ObjectGuid Eluna::CHECKVAL<ObjectGuid>(int narg)
{
    ObjectGuid* guid = CHECKOBJ<ObjectGuid>(narg, true);
    return guid ? *guid : ObjectGuid();
}

template<> Object* Eluna::CHECKOBJ<Object>(int narg, bool error)
{
    Object* obj = CHECKOBJ<WorldObject>(narg, false);
    if (!obj)
        obj = CHECKOBJ<Item>(narg, false);
    if (!obj)
        obj = ElunaTemplate<Object>::Check(this, narg, error);
    return obj;
}
template<> WorldObject* Eluna::CHECKOBJ<WorldObject>(int narg, bool error)
{
    WorldObject* obj = CHECKOBJ<Unit>(narg, false);
    if (!obj)
        obj = CHECKOBJ<GameObject>(narg, false);
    if (!obj)
        obj = CHECKOBJ<Corpse>(narg, false);
    if (!obj)
        obj = ElunaTemplate<WorldObject>::Check(this, narg, error);
    return obj;
}
template<> Unit* Eluna::CHECKOBJ<Unit>(int narg, bool error)
{
    Unit* obj = CHECKOBJ<Player>(narg, false);
    if (!obj)
        obj = CHECKOBJ<Creature>(narg, false);
    if (!obj)
        obj = ElunaTemplate<Unit>::Check(this, narg, error);
    return obj;
}

template<> ElunaObject* Eluna::CHECKOBJ<ElunaObject>(int narg, bool error)
{
    return CHECKTYPE(narg, NULL, error);
}

ElunaObject* Eluna::CHECKTYPE(int narg, const char* tname, bool error)
{
    if (lua_islightuserdata(L, narg))
    {
        if (error)
            luaL_argerror(L, narg, "bad argument : userdata expected, got lightuserdata");
        return NULL;
    }

    ElunaObject* elunaObject = static_cast<ElunaObject*>(lua_touserdata(L, narg));

    if (!elunaObject || (tname && elunaObject->GetTypeName() != tname))
    {
        if (error)
        {
            char buff[256];
            snprintf(buff, 256, "bad argument : %s expected, got %s", tname ? tname : "ElunaObject", elunaObject ? elunaObject->GetTypeName() : luaL_typename(L, narg));
            luaL_argerror(L, narg, buff);
        }
        return NULL;
    }
    return elunaObject;
}

template<typename K>
static int cancelBinding(lua_State* L)
{
    Eluna* E = Eluna::GetEluna(L);

    uint64 bindingID = E->CHECKVAL<uint64>(lua_upvalueindex(1));

    BindingMap<K>* bindings = (BindingMap<K>*)lua_touserdata(L, lua_upvalueindex(2));
    ASSERT(bindings != NULL);

    bindings->Remove(bindingID);

    return 0;
}

template<typename K>
static void createCancelCallback(Eluna* e, uint64 bindingID, BindingMap<K>* bindings)
{
    e->Push(bindingID);
    lua_pushlightuserdata(e->L, bindings);
    // Stack: bindingID, bindings

    lua_pushcclosure(e->L, &cancelBinding<K>, 2);
    // Stack: cancel_callback
}

template<typename K>
int RegisterBasicBinding(Eluna* e, std::underlying_type_t<Hooks::RegisterTypes> regtype, uint32 event_id, int functionRef, uint32 shots)
{
    typedef EventKey<K> Key;
    auto binding = e->GetBinding<Key>(regtype);
    auto key = Key(static_cast<K>(event_id));
    uint64 bindingID = binding->Insert(key, functionRef, shots);
    createCancelCallback(e, bindingID, binding);
    return 1; // Stack: callback
}

template<typename K>
int RegisterEntryBinding(Eluna* e, std::underlying_type_t<Hooks::RegisterTypes> regtype, uint32 entry, uint32 event_id, int functionRef, uint32 shots)
{
    typedef EntryKey<K> Key;
    auto binding = e->GetBinding<Key>(regtype);
    auto key = Key(static_cast<K>(event_id), entry);
    uint64 bindingID = binding->Insert(key, functionRef, shots);
    createCancelCallback(e, bindingID, binding);
    return 1; // Stack: callback
}

template<typename K>
int RegisterUniqueBinding(Eluna* e, std::underlying_type_t<Hooks::RegisterTypes> regtype, ObjectGuid guid, uint32 instanceId, uint32 event_id, int functionRef, uint32 shots)
{
    typedef UniqueObjectKey<K> Key;
    auto binding = e->GetBinding<Key>(regtype);
    auto key = Key(static_cast<K>(event_id), guid, instanceId);
    uint64 bindingID = binding->Insert(key, functionRef, shots);
    createCancelCallback(e, bindingID, binding);
    return 1; // Stack: callback
}

// Saves the function reference ID given to the register type's store for given entry under the given event
int Eluna::Register(std::underlying_type_t<Hooks::RegisterTypes> regtype, uint32 entry, ObjectGuid guid, uint32 instanceId, uint32 event_id, int functionRef, uint32 shots)
{
    switch (regtype)
    {
        case Hooks::REGTYPE_SERVER:
            if (event_id < Hooks::SERVER_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::ServerEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_PLAYER:
            if (event_id < Hooks::PLAYER_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::PlayerEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_GUILD:
            if (event_id < Hooks::GUILD_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::GuildEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_GROUP:
            if (event_id < Hooks::GROUP_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::GroupEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_VEHICLE:
            if (event_id < Hooks::VEHICLE_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::VehicleEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_BG:
            if (event_id < Hooks::BG_EVENT_COUNT)
                return RegisterBasicBinding<Hooks::BGEvents>(this, regtype, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_PACKET:
            if (event_id < Hooks::PACKET_EVENT_COUNT)
            {
                if (entry >= NUM_MSG_TYPES)
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a creature with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::PacketEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_CREATURE:
            if (event_id < Hooks::CREATURE_EVENT_COUNT)
            {
                if (!eObjectMgr->GetCreatureTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a creature with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::CreatureEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_CREATURE_UNIQUE:
            if (event_id < Hooks::CREATURE_EVENT_COUNT)
            {
                if (guid.IsEmpty())
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "guid was 0!");
                    return 0; // Stack: (empty)
                }
                return RegisterUniqueBinding<Hooks::CreatureEvents>(this, regtype, guid, instanceId, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_CREATURE_GOSSIP:
            if (event_id < Hooks::GOSSIP_EVENT_COUNT)
            {
                if (!eObjectMgr->GetCreatureTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a creature with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::GossipEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_GAMEOBJECT:
            if (event_id < Hooks::GAMEOBJECT_EVENT_COUNT)
            {
                if (!eObjectMgr->GetGameObjectTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a gameobject with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::GameObjectEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_GAMEOBJECT_GOSSIP:
            if (event_id < Hooks::GOSSIP_EVENT_COUNT)
            {
                if (!eObjectMgr->GetGameObjectTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a gameobject with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::GossipEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_SPELL:
            if (event_id < Hooks::SPELL_EVENT_COUNT)
                return RegisterEntryBinding<Hooks::SpellEvents>(this, regtype, entry, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_ITEM:
            if (event_id < Hooks::ITEM_EVENT_COUNT)
            {
                if (!eObjectMgr->GetItemTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a item with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::ItemEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_ITEM_GOSSIP:
            if (event_id < Hooks::GOSSIP_EVENT_COUNT)
            {
                if (!eObjectMgr->GetItemTemplate(entry))
                {
                    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
                    luaL_error(L, "Couldn't find a item with (ID: %d)!", entry);
                    return 0; // Stack: (empty)
                }
                return RegisterEntryBinding<Hooks::GossipEvents>(this, regtype, entry, event_id, functionRef, shots);
            }
            break;

        case Hooks::REGTYPE_PLAYER_GOSSIP:
            if (event_id < Hooks::GOSSIP_EVENT_COUNT)
                return RegisterEntryBinding<Hooks::GossipEvents>(this, regtype, entry, event_id, functionRef, shots);
            break;

        case Hooks::REGTYPE_MAP:
        case Hooks::REGTYPE_INSTANCE:
            if (event_id < Hooks::INSTANCE_EVENT_COUNT)
                return RegisterEntryBinding<Hooks::InstanceEvents>(this, regtype, entry, event_id, functionRef, shots);
            break;
    }
    luaL_unref(L, LUA_REGISTRYINDEX, functionRef);
    std::ostringstream oss;
    oss << "regtype " << static_cast<unsigned int>(regtype) << ", event " << event_id << ", entry " << entry << ", guid " <<
#if defined ELUNA_TRINITY
        guid.ToHexString()
#else
        guid.GetRawValue()
#endif
        << ", instance " << instanceId;
    luaL_error(L, "Unknown event type (%s)", oss.str().c_str());
    return 0;
}

void Eluna::UpdateEluna(uint32 diff)
{
    if (reload && sElunaLoader->GetCacheState() == SCRIPT_CACHE_READY)
#if defined ELUNA_TRINITY
        if (GetQueryProcessor().Empty())
#endif
            _ReloadEluna();

    eventMgr->UpdateProcessors(diff);
#if defined ELUNA_TRINITY
    GetQueryProcessor().ProcessReadyCallbacks();
#endif
}

/*
 * Cleans up the stack, effectively undoing all Push calls and the Setup call.
 */
void Eluna::CleanUpStack(int number_of_arguments)
{
    // Stack: event_id, [arguments]

    lua_pop(L, number_of_arguments + 1); // Add 1 because the caller doesn't know about `event_id`.
    // Stack: (empty)

#if !defined TRACKABLE_PTR_NAMESPACE
    if (event_level == 0)
        InvalidateObjects();
#endif
}

/*
 * Call a single event handler that was put on the stack with `Setup` and removes it from the stack.
 *
 * The caller is responsible for keeping track of how many times this should be called.
 */
int Eluna::CallOneFunction(int number_of_functions, int number_of_arguments, int number_of_results)
{
    ++number_of_arguments; // Caller doesn't know about `event_id`.
    ASSERT(number_of_functions > 0 && number_of_arguments > 0 && number_of_results >= 0);
    // Stack: event_id, [arguments], [functions]

    int functions_top        = lua_gettop(L);
    int first_function_index = functions_top - number_of_functions + 1;
    int arguments_top        = first_function_index - 1;
    int first_argument_index = arguments_top - number_of_arguments + 1;

    // Copy the arguments from the bottom of the stack to the top.
    for (int argument_index = first_argument_index; argument_index <= arguments_top; ++argument_index)
    {
        lua_pushvalue(L, argument_index);
    }
    // Stack: event_id, [arguments], [functions], event_id, [arguments]

    ExecuteCall(number_of_arguments, number_of_results);
    --functions_top;
    // Stack: event_id, [arguments], [functions - 1], [results]

    return functions_top + 1; // Return the location of the first result (if any exist).
}

CreatureAI* Eluna::GetAI(Creature* creature)
{
    for (int i = 1; i < Hooks::CREATURE_EVENT_COUNT; ++i)
    {
        Hooks::CreatureEvents event_id = (Hooks::CreatureEvents)i;

        typedef EntryKey<Hooks::CreatureEvents> EKey;
        typedef UniqueObjectKey<Hooks::CreatureEvents> UKey;

        auto entryKey = EKey(event_id, creature->GetEntry());
        auto uniqueKey = UKey(event_id, creature->GET_GUID(), creature->GetInstanceId());

        auto CreatureEBindings = GetBinding<EKey>(Hooks::REGTYPE_CREATURE);
        auto CreatureUBindings = GetBinding<UKey>(Hooks::REGTYPE_CREATURE_UNIQUE);

        if (CreatureEBindings->HasBindingsFor(entryKey) ||
            CreatureUBindings->HasBindingsFor(uniqueKey))
            return new ElunaCreatureAI(creature);
    }

    return NULL;
}

InstanceData* Eluna::GetInstanceData(Map* map)
{
    for (int i = 1; i < Hooks::INSTANCE_EVENT_COUNT; ++i)
    {
        Hooks::InstanceEvents event_id = (Hooks::InstanceEvents)i;

        typedef EntryKey<Hooks::InstanceEvents> Key;

        auto key = Key(event_id, map->GetId());

        auto MapBindings = GetBinding<Key>(Hooks::REGTYPE_MAP);
        auto InstanceBindings = GetBinding<Key>(Hooks::REGTYPE_INSTANCE);

        if (MapBindings->HasBindingsFor(key) ||
            InstanceBindings->HasBindingsFor(key))
            return new ElunaInstanceAI(map);
    }

    return NULL;
}

bool Eluna::HasInstanceData(Map const* map)
{
    if (!map->Instanceable())
        return continentDataRefs.find(map->GetId()) != continentDataRefs.end();
    else
        return instanceDataRefs.find(map->GetInstanceId()) != instanceDataRefs.end();
}

void Eluna::CreateInstanceData(Map const* map)
{
    ASSERT(lua_istable(L, -1));
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    if (!map->Instanceable())
    {
        uint32 mapId = map->GetId();

        // If there's another table that was already stored for the map, unref it.
        auto mapRef = continentDataRefs.find(mapId);
        if (mapRef != continentDataRefs.end())
        {
            luaL_unref(L, LUA_REGISTRYINDEX, mapRef->second);
        }

        continentDataRefs[mapId] = ref;
    }
    else
    {
        uint32 instanceId = map->GetInstanceId();

        // If there's another table that was already stored for the instance, unref it.
        auto instRef = instanceDataRefs.find(instanceId);
        if (instRef != instanceDataRefs.end())
        {
            luaL_unref(L, LUA_REGISTRYINDEX, instRef->second);
        }

        instanceDataRefs[instanceId] = ref;
    }
}

/*
 * Unrefs the instanceId related events and data
 * Does all required actions for when an instance is freed.
 */
void Eluna::FreeInstanceId(uint32 instanceId)
{
    for (int i = 1; i < Hooks::INSTANCE_EVENT_COUNT; ++i)
    {
        typedef EntryKey<Hooks::InstanceEvents> Key;

        auto key = Key((Hooks::InstanceEvents)i, instanceId);

        auto MapEventBindings = GetBinding<Key>(Hooks::REGTYPE_MAP);
        auto InstanceEventBindings = GetBinding<Key>(Hooks::REGTYPE_INSTANCE);

        if (MapEventBindings->HasBindingsFor(key))
            MapEventBindings->Clear(key);

        if (InstanceEventBindings->HasBindingsFor(key))
            InstanceEventBindings->Clear(key);

        if (instanceDataRefs.find(instanceId) != instanceDataRefs.end())
        {
            luaL_unref(L, LUA_REGISTRYINDEX, instanceDataRefs[instanceId]);
            instanceDataRefs.erase(instanceId);
        }
    }
}

void Eluna::PushInstanceData(ElunaInstanceAI* ai, bool incrementCounter)
{
    // Check if the instance data is missing (i.e. someone reloaded Eluna).
    if (!HasInstanceData(ai->instance))
        ai->Reload();

    // Get the instance data table from the registry.
    if (!ai->instance->Instanceable())
        lua_rawgeti(L, LUA_REGISTRYINDEX, continentDataRefs[ai->instance->GetId()]);
    else
        lua_rawgeti(L, LUA_REGISTRYINDEX, instanceDataRefs[ai->instance->GetInstanceId()]);

    ASSERT(lua_istable(L, -1));

    if (incrementCounter)
        ++push_counter;
}
