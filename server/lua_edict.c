/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// sv_edict.c -- entity dictionary

#include "qwsvdef.h"

lua_State *L;

// shared compatibility variables
int num_prstr;
dprograms_t *progs;
char *pr_strings;
globalvars_t *pr_global_struct;
int pr_edict_size; // in bytes

func_t SpectatorConnect;
func_t SpectatorThink;
func_t SpectatorDisconnect;

static void ED_EnsureFields(edict_t *ed)
{
    if (ed->ref == 0) {
        edict_t **ud = lua_newuserdata(L, sizeof(void*));
        *ud = ed;
        luaL_getmetatable(L, "edict_t");
        lua_setmetatable(L, -2);
        ed->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (ed->fields == 0) {
        lua_newtable(L);
        ed->fields = luaL_ref(L, LUA_REGISTRYINDEX);
    }
}


/*
=================
ED_ClearEdict

Sets everything to NULL
=================
*/
void ED_ClearEdict(edict_t * e)
{
    e->free = false;

    memset(&e->v, 0, sizeof(entvars_t));

    if (e->fields)
        luaL_unref(L, LUA_REGISTRYINDEX, e->fields);

    e->fields = 0;
    ED_EnsureFields(e);
}

/*
=================
ED_Alloc

Either finds a free edict, or allocates a new one.
Try to avoid reusing an entity that was recently freed, because it
can cause the client to think the entity morphed into something else
instead of being removed and recreated, which can cause interpolated
angles and bad trails.
=================
*/
edict_t *ED_Alloc(void)
{
    int i;
    edict_t *e;

    for (i = MAX_CLIENTS + 1; i < sv.num_edicts; i++) {
        e = EDICT_NUM(i);
        // the first couple seconds of server time can involve a lot of
        // freeing and allocating, so relax the replacement policy
        if (e->free && (e->freetime < 2 || sv.time - e->freetime > 0.5)) {
            ED_ClearEdict(e);
            return e;
        }
    }

    if (i == MAX_EDICTS) {
        Con_Printf("WARNING: ED_Alloc: no free edicts\n");
        i--;                    // step on whatever is the last edict
        e = EDICT_NUM(i);
        SV_UnlinkEdict(e);
    } else
        sv.num_edicts++;
    e = EDICT_NUM(i);
    ED_ClearEdict(e);

    return e;
}

#define FREE_REF(n) \
    if (e->v.n) { \
        luaL_unref(L, LUA_REGISTRYINDEX, e->v.n); \
        e->v.n = 0; \
    }

/*
=================
ED_Free

Marks the edict as free
FIXME: walk all entities and NULL out references to this entity
=================
*/
void ED_Free(edict_t * e)
{
    SV_UnlinkEdict(e); // unlink from world bsp

    FREE_REF(classname);
    FREE_REF(model);
    FREE_REF(touch);
    FREE_REF(use);
    FREE_REF(think);
    FREE_REF(blocked);
    FREE_REF(weaponmodel);
    FREE_REF(netname);
    FREE_REF(target);
    FREE_REF(targetname);
    FREE_REF(message);
    FREE_REF(noise);
    FREE_REF(noise1);
    FREE_REF(noise2);
    FREE_REF(noise3);

    e->free = true;
    e->v.model = 0;
    e->v.takedamage = 0;
    e->v.modelindex = 0;
    e->v.colormap = 0;
    e->v.skin = 0;
    e->v.frame = 0;
    VectorCopy(vec3_origin, e->v.origin);
    VectorCopy(vec3_origin, e->v.angles);
    e->v.nextthink = -1;
    e->v.solid = 0;

    e->freetime = sv.time;
}

//===========================================================================
/*
=============
ED_SetField

Tries to guess the value type and sets an edict field to that.

Note: Expects the Lua stack to have a table.
Note: This will not support savegames.
=============
*/

static double* str_tonumber(const char *s)
{
    static double d;

    if (sscanf(s, "%lf", &d) == 1)
        return &d;

    return NULL;
}

static vec_t* str_tovector(const char *s)
{
    static vec3_t v;

    if (sscanf(s, "%f %f %f", &v[0], &v[1], &v[2]) == 3)
        return v;

    return NULL;
}

qboolean ED_SetField(edict_t *e, const char *key, char *value)
{
    int i;
    char string[128];
    char *v, *w;
    vec_t *vec, *nvec;
    double *num;

    // badly unescape NL in value
    v = w = value;
    while (*v != '\0') {
        if (v > value && *v == 'n' && *(v - 1) == '\\') {
            *(w-1) = '\n';
            v++;
        } else {
            *w++ = *v++;
        }
    }
    *w = '\0';

    // first handle C fields
    FIELD_FLOAT(sounds);
    FIELD_STRING(classname);
    FIELD_STRING(message);
    FIELD_VEC(origin);
    FIELD_VEC(angles);
    FIELD_STRING(target);
    FIELD_STRING(model);
    FIELD_STRING(targetname);
    FIELD_FLOAT(spawnflags);
    FIELD_FLOAT(health);

    lua_rawgeti(L, LUA_REGISTRYINDEX, e->fields);
    lua_pushstring(L, key);

    if ((vec = str_tovector(value))) {
        nvec = PR_Vec3_New(L);
        memcpy(nvec, vec, sizeof(vec3_t));
    } else if ((num = str_tonumber(value))) {
        lua_pushnumber(L, *num);
    } else {
        lua_pushstring(L, value);
    }

    lua_rawset(L, -3);
    lua_pop(L, 1);
    return true;
}

/*
====================
ED_ParseEdict

Parses an edict out of the given string, returning the new position
ed should be a properly initialized empty edict.
Used for initial level load and for savegames.
====================
*/
char *ED_ParseEdict(char *data, edict_t * ent)
{
    qboolean anglehack;
    qboolean init;
    char keyname[256];

    init = false;

    // clear it
    if (ent != sv.edicts) // XXX: refs!
        memset(&ent->v, 0, sizeof(entvars_t));

    // go through all the dictionary pairs
    while (1) {
        // parse key
        data = COM_Parse(data);
        if (com_token[0] == '}')
            break;
        if (!data)
            SV_Error("ED_ParseEntity: EOF without closing brace");

        // anglehack is to allow QuakeEd to write single scalar angles
        // and allow them to be turned into vectors. (FIXME...)
        if (!strcmp(com_token, "angle")) {
            strcpy(com_token, "angles");
            anglehack = true;
        } else
            anglehack = false;

        // FIXME: change light to _light to get rid of this hack
        if (!strcmp(com_token, "light"))
            strcpy(com_token, "light_lev");     // hack for single light def

        strcpy(keyname, com_token);

        // parse value  
        data = COM_Parse(data);
        if (!data)
            SV_Error("ED_ParseEntity: EOF without closing brace");

        if (com_token[0] == '}')
            SV_Error("ED_ParseEntity: closing brace without data");

        init = true;

        // keynames with a leading underscore are used for utility comments,
        // and are immediately discarded by quake
        if (keyname[0] == '_')
            continue;

        if (anglehack) {
            char temp[32];
            strcpy(temp, com_token);
            sprintf(com_token, "0 %s 0", temp);
        }

        if (!ED_SetField(ent, keyname, com_token))
            SV_Error("ED_ParseEdict: parse error, can't set field '%s' to '%s'", keyname, com_token);
    }

    if (!init)
        ent->free = true;

    return data;
}

/*
================
ED_LoadFromFile

The entities are directly placed in the array, rather than allocated with
ED_Alloc, because otherwise an error loading the map would have entity
number references out of order.

Creates a server's entity / program execution context by
parsing textual entity definitions out of an ent file.

Used for both fresh maps and savegame loads.  A fresh map would also need
to call ED_CallSpawnFunctions () to let the objects initialize themselves.
================
*/
void ED_LoadFromFile(char *data)
{
    edict_t *ent;
    int inhibit;
    int ref;
    int i;

    Con_Printf("ED_LoadFromFile(data=%p)\n", data);

    ent = NULL;
    inhibit = 0;

    // XXX: this is a stupid place to do this
    for (i = 1; i <= MAX_CLIENTS; i++) {
        ED_EnsureFields(EDICT_NUM(i));
    }

    // parse ents
    while (1) {
        // parse the opening brace      
        data = COM_Parse(data);
        if (!data)
            break;
        if (com_token[0] != '{')
            SV_Error("ED_LoadFromFile: found %s when expecting {",
                     com_token);

        if (!ent)
            ent = EDICT_NUM(0);
        else
            ent = ED_Alloc();
        data = ED_ParseEdict(data, ent);

        // remove things from different skill levels or deathmatch
#if 0
        if (((int) ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH)) {
            ED_Free(ent);
            inhibit++;
            continue;
        }
#else
        #define current_skill 0 // XXX
        if (deathmatch.value)
        {
            if (((int)ent->v.spawnflags & SPAWNFLAG_NOT_DEATHMATCH))
            {
                ED_Free (ent);  
                inhibit++;
                continue;
            }
        }
        else if ((current_skill == 0 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_EASY))
            || (current_skill == 1 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_MEDIUM))
            || (current_skill >= 2 && ((int)ent->v.spawnflags & SPAWNFLAG_NOT_HARD)) )
        {
            ED_Free (ent);  
            inhibit++;
            continue;
        }
#endif
        //
        // immediately call spawn function
        //
        if (!ent->v.classname) {
            Con_Printf("No classname for:\n");
            //ED_Print(ent);
            ED_Free(ent);
            continue;
        }
        // look for the spawn function
        lua_getglobal(L, PR_GetString(ent->v.classname));

        if (!lua_isfunction(L, -1)) {
            Con_Printf("No spawn function for '%s'\n", PR_GetString(ent->v.classname));
            //ED_Print(ent);
            ED_Free(ent);
            lua_pop(L, 1);
            continue;
        }

        pr_global_struct->self = ent->ref;

        ref = luaL_ref(L, LUA_REGISTRYINDEX);
        PR_ExecuteProgram(ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ref);

        SV_FlushSignon();
    }

    Con_DPrintf("%i entities inhibited\n", inhibit);
}

eval_t *GetEdictFieldValue(edict_t * ed, char *field)
{
    //Con_Printf("GetEdictFieldValue(edict=%p, field=\"%s\")\n", ed, field);
    return NULL;
}

int ED_FindFunction(const char *name)
{
    lua_getglobal(L, name);

    if (lua_isfunction(L, -1))
        return luaL_ref(L, LUA_REGISTRYINDEX);

    Con_Printf("Did not find function '%s'\n", name);

    lua_pop(L, 1);
    return LUA_NOREF;
}

#define PUSH_BOOLEAN(s) \
    if (strcmp(key, #s) == 0) { \
        lua_pushboolean(L, (*e)->v.s); \
        return 1; \
    }

#define PUSH_FLOAT(s) \
    if (strcmp(key, #s) == 0) { \
        lua_pushnumber(L, (*e)->v.s); \
        return 1; \
    }

#define PUSH_REF(s) \
    if (strcmp(key, #s) == 0) { \
        if ((*e)->v.s == 0) \
            lua_pushnil(L); \
        else \
            lua_rawgeti(L, LUA_REGISTRYINDEX, (*e)->v.s); \
        return 1; \
    }

#define PUSH_VEC3(s) \
    if (strcmp(key, #s) == 0) { \
        PR_Vec3_Push(L, (*e)->v.s); \
        return 1; \
    }

static int ED_mt_index(lua_State *L)
{
    edict_t **e;
    const char *key;

    e = luaL_checkudata(L, 1, "edict_t");
    key = lua_tostring(L, 2);

    // first handle C fields
    PUSH_FLOAT(modelindex);
    PUSH_VEC3(absmin);
    PUSH_VEC3(absmax);
    PUSH_FLOAT(ltime);
    PUSH_FLOAT(lastruntime);
    PUSH_FLOAT(movetype);
    PUSH_FLOAT(solid);
    PUSH_VEC3(origin);
    PUSH_VEC3(oldorigin);
    PUSH_VEC3(velocity);
    PUSH_VEC3(angles);
    PUSH_VEC3(avelocity);
    PUSH_REF(classname);
    PUSH_REF(model);
    PUSH_FLOAT(frame);
    PUSH_FLOAT(skin);
    PUSH_FLOAT(effects);
    PUSH_VEC3(mins);
    PUSH_VEC3(maxs);
    PUSH_VEC3(size);
    PUSH_REF(touch);
    PUSH_REF(use);
    PUSH_REF(think);
    PUSH_REF(blocked);
    PUSH_FLOAT(nextthink);
    PUSH_REF(groundentity);
    PUSH_FLOAT(health);
    PUSH_FLOAT(frags);
    PUSH_FLOAT(weapon);
    PUSH_REF(weaponmodel);
    PUSH_FLOAT(weaponframe);
    PUSH_FLOAT(currentammo);
    PUSH_FLOAT(ammo_shells);
    PUSH_FLOAT(ammo_nails);
    PUSH_FLOAT(ammo_rockets);
    PUSH_FLOAT(ammo_cells);
    PUSH_FLOAT(items);
    PUSH_FLOAT(takedamage);
    PUSH_REF(chain);
    PUSH_FLOAT(deadflag);
    PUSH_VEC3(view_ofs);
    PUSH_FLOAT(button0);
    PUSH_FLOAT(button1);
    PUSH_FLOAT(button2);
    PUSH_FLOAT(impulse);
    PUSH_BOOLEAN(fixangle);
    PUSH_VEC3(v_angle);
    PUSH_REF(netname);
    PUSH_REF(enemy);
    PUSH_FLOAT(flags);
    PUSH_FLOAT(colormap);
    PUSH_FLOAT(team);
    PUSH_FLOAT(max_health);
    PUSH_FLOAT(teleport_time);
    PUSH_FLOAT(armortype);
    PUSH_FLOAT(armorvalue);
    PUSH_FLOAT(waterlevel);
    PUSH_FLOAT(watertype);
    PUSH_FLOAT(ideal_yaw);
    PUSH_FLOAT(yaw_speed);
    PUSH_REF(aiment);
    PUSH_REF(goalentity);
    PUSH_FLOAT(spawnflags);
    PUSH_REF(target);
    PUSH_REF(targetname);
    PUSH_FLOAT(dmg_take);
    PUSH_FLOAT(dmg_save);
    PUSH_REF(dmg_inflictor);
    PUSH_REF(owner);
    PUSH_VEC3(movedir);
    PUSH_REF(message);
    PUSH_FLOAT(sounds);
    PUSH_REF(noise);
    PUSH_REF(noise1);
    PUSH_REF(noise2);
    PUSH_REF(noise3);

    lua_rawgeti(L, LUA_REGISTRYINDEX, (*e)->fields);
    lua_pushstring(L, key);
    lua_rawget(L, -2);
    lua_remove(L, -2);

    return 1;
}

#define SET_BOOLEAN(s)  \
    if (strcmp(key, #s) == 0) { \
        luaL_checktype(L, 3, LUA_TBOOLEAN); \
        (*e)->v.s = lua_toboolean(L, 3); \
        return 0; \
    }

#define SET_FLOAT(s)  \
    if (strcmp(key, #s) == 0) { \
        (*e)->v.s = luaL_checknumber(L, 3); \
        return 0; \
    }

#define SET_VEC3(s) \
    if (strcmp(key, #s) == 0) { \
        vec_t *_tmpvec; \
        _tmpvec = PR_Vec3_ToVec(L, 3); \
        memcpy((*e)->v.s, _tmpvec, sizeof(vec3_t)); \
        return 0; \
    }

#define SET_REF(s)  \
    if (strcmp(key, #s) == 0) { \
        if ((*e)->v.s) luaL_unref(L, LUA_REGISTRYINDEX, (*e)->v.s); \
        (*e)->v.s = 0; \
        if (!lua_isnil(L, 3)) { \
            lua_pushvalue(L, 3); \
            (*e)->v.s = luaL_ref(L, LUA_REGISTRYINDEX); \
        } \
        return 0; \
    }

#define SET_EDICT(s)  \
    if (strcmp(key, #s) == 0) { \
        (*e)->v.s = 0; \
        if (!lua_isnil(L, 3)) { \
            e2 = luaL_checkudata(L, 3, "edict_t"); \
            (*e)->v.s = (*e2)->ref; \
        } \
        return 0; \
    }

static int ED_mt_newindex(lua_State *L)
{
    edict_t **e, **e2;
    const char *key;

    e = luaL_checkudata(L, 1, "edict_t");
    key = lua_tostring(L, 2);

    // first handle C fields
    SET_FLOAT(modelindex);
    SET_VEC3(absmin);
    SET_VEC3(absmax);
    SET_FLOAT(ltime);
    SET_FLOAT(lastruntime);
    SET_FLOAT(movetype);
    SET_FLOAT(solid);
    SET_VEC3(origin);
    SET_VEC3(oldorigin);
    SET_VEC3(velocity);
    SET_VEC3(angles);
    SET_VEC3(avelocity);
    SET_REF(classname);
    SET_REF(model);
    SET_FLOAT(frame);
    SET_FLOAT(skin);
    SET_FLOAT(effects);
    SET_VEC3(mins);
    SET_VEC3(maxs);
    SET_VEC3(size);
    SET_REF(touch);
    SET_REF(use);
    SET_REF(think);
    SET_REF(blocked);
    SET_FLOAT(nextthink);
    SET_EDICT(groundentity);
    SET_FLOAT(health);
    SET_FLOAT(frags);
    SET_FLOAT(weapon);
    SET_REF(weaponmodel);
    SET_FLOAT(weaponframe);
    SET_FLOAT(currentammo);
    SET_FLOAT(ammo_shells);
    SET_FLOAT(ammo_nails);
    SET_FLOAT(ammo_rockets);
    SET_FLOAT(ammo_cells);
    SET_FLOAT(items);
    SET_FLOAT(takedamage);
    SET_EDICT(chain);
    SET_FLOAT(deadflag);
    SET_VEC3(view_ofs);
    SET_FLOAT(button0);
    SET_FLOAT(button1);
    SET_FLOAT(button2);
    SET_FLOAT(impulse);
    SET_BOOLEAN(fixangle);
    SET_VEC3(v_angle);
    SET_REF(netname);
    SET_EDICT(enemy);
    SET_FLOAT(flags);
    SET_FLOAT(colormap);
    SET_FLOAT(team);
    SET_FLOAT(max_health);
    SET_FLOAT(teleport_time);
    SET_FLOAT(armortype);
    SET_FLOAT(armorvalue);
    SET_FLOAT(waterlevel);
    SET_FLOAT(watertype);
    SET_FLOAT(ideal_yaw);
    SET_FLOAT(yaw_speed);
    SET_EDICT(aiment);
    SET_EDICT(goalentity);
    SET_FLOAT(spawnflags);
    SET_REF(target);
    SET_REF(targetname);
    SET_FLOAT(dmg_take);
    SET_FLOAT(dmg_save);
    SET_EDICT(dmg_inflictor);
    SET_EDICT(owner);
    SET_VEC3(movedir);
    SET_REF(message);
    SET_FLOAT(sounds);
    SET_REF(noise);
    SET_REF(noise1);
    SET_REF(noise2);
    SET_REF(noise3);

    lua_rawgeti(L, LUA_REGISTRYINDEX, (*e)->fields);
    lua_pushstring(L, key);

    // deep copy of vec3_t when assigning
    if (luaL_testudata(L, 3, "vec3_t")) {
        vec_t *ovec, *nvec;
        ovec = PR_Vec3_ToVec(L, 3);
        nvec = PR_Vec3_New(L);
        memcpy(nvec, ovec, sizeof(vec3_t));
    } else {
        lua_pushvalue(L, 3);
    }

    lua_rawset(L, -3);
    lua_pop(L, 1);

    return 0;
}

static int ED_mt_tostring(lua_State *L)
{
    static char buf[32];
    edict_t **e;

    e = luaL_checkudata(L, 1, "edict_t");
    snprintf(buf, sizeof(buf), "edict_t %p", *e);

    lua_pushstring(L, buf);
    return 1;
}

static const luaL_Reg ED_mt[] = {
    {"__index",     ED_mt_index},
    {"__newindex",  ED_mt_newindex},
    {"__tostring",  ED_mt_tostring},
    {0, 0}
};

/*
===============
PR_LoadProgs
===============
*/
void PR_LoadProgs(void)
{
    const char *path;
    char *buf;
    byte* code;

    // shared state
    pr_global_struct = Z_Malloc(sizeof *pr_global_struct);

    // sv_init.c compatibility
    pr_edict_size = sizeof(edict_t);
    pr_strings = ""; // uh?

    // sv_user.c compatibility
    progs = Z_Malloc(sizeof *progs);
    progs->entityfields = sizeof(((edict_t *)0)->v) / 4;

    // sv_ccmds.c compatibility
    num_prstr = 0;

    L = luaL_newstate();
    luaL_openlibs(L);

    // weird trick to append to packages.path
    lua_newtable(L);
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "path");
    path = lua_tostring(L, -1);

    buf = Z_Malloc(strlen(path) + strlen(com_gamedir) + 8);
    sprintf(buf, "%s;%s/?.lua", path, com_gamedir);

    lua_pushstring(L, buf);
    lua_setfield(L, -3, "path");
    lua_pop(L, 2);

    PR_Vec3_Init(L);

    luaL_newmetatable(L, "edict_t");
    luaL_setfuncs(L, ED_mt, 0);
    lua_pop(L, 1);

    PR_InstallBuiltins();

    code = COM_LoadHunkFile("qwprogs.lua");
    if (!code)
        SV_Error("No qwprogs.lua found.");

    if (luaL_loadstring(L, (char *)code) != LUA_OK)
        SV_Error((char *)lua_tostring(L, -1));

    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        SV_Error((char *)lua_tostring(L, -1));

    pr_global_struct->main = ED_FindFunction("main");
    pr_global_struct->StartFrame = ED_FindFunction("StartFrame");
    pr_global_struct->PlayerPreThink = ED_FindFunction("PlayerPreThink");
    pr_global_struct->PlayerPostThink = ED_FindFunction("PlayerPostThink");
    pr_global_struct->ClientKill = ED_FindFunction("ClientKill");
    pr_global_struct->ClientConnect = ED_FindFunction("ClientConnect");
    pr_global_struct->PutClientInServer = ED_FindFunction("PutClientInServer");
    pr_global_struct->ClientDisconnect = ED_FindFunction("ClientDisconnect");
    pr_global_struct->SetNewParms = ED_FindFunction("SetNewParms");
    pr_global_struct->SetChangeParms = ED_FindFunction("SetChangeParms");

    SpectatorConnect = SpectatorThink = SpectatorDisconnect = 0;

    SpectatorConnect = ED_FindFunction("SpectatorConnect");
    SpectatorThink = ED_FindFunction("SpectatorThink");
    SpectatorDisconnect = ED_FindFunction("SpectatorDisconnect");
}


/*
===============
PR_Init
===============
*/
void PR_Init(void)
{
    Con_Printf("PR_Init called\n");
    /*
    Cmd_AddCommand("edict", ED_PrintEdict_f);
    Cmd_AddCommand("edicts", ED_PrintEdicts);
    Cmd_AddCommand("edictcount", ED_Count);
    Cmd_AddCommand("profile", PR_Profile_f);
    */
}

/*
====================
PR_ExecuteProgram
====================
*/
void PR_ExecuteProgram(func_t fnum)
{
    // if thinking without a valid function, we still get called
    if (fnum == 0)
        return;

    if (fnum < 0)
        SV_Error("PR_ExecuteProgram(%d) got invalid fnum, this is a bug.\n", fnum);

    lua_rawgeti(L, LUA_REGISTRYINDEX, fnum);

    if (!lua_isfunction(L, -1))
        SV_Error("PR_ExecuteProgram(%d) did not get a function, got '%s' instead", fnum, lua_typename(L, -1));

    // XXX: big hack because first frame is run before other edicts are initialized than world
    if (sv.state == ss_loading && EDICT_NUM(0)->ref == 0) {
        ED_EnsureFields(EDICT_NUM(0));
        pr_global_struct->self = EDICT_NUM(0)->ref;
        pr_global_struct->other = EDICT_NUM(0)->ref;
        pr_global_struct->world = EDICT_NUM(0)->ref;

        PUSH_GREF(world);
        PUSH_GREF(mapname);
        PUSH_GFLOAT(serverflags);

        // push them but we ignore the values for now
        PUSH_GFLOAT(total_secrets);
        PUSH_GFLOAT(total_monsters);
        PUSH_GFLOAT(found_secrets);
        PUSH_GFLOAT(killed_monsters);
    }

    if (fnum == pr_global_struct->StartFrame) {
        GET_GFLOAT(force_retouch); // this should be fine
    }

    if (pr_global_struct->self == 0)
        SV_Error("Executing a function with zero self, this is a bug.\n");

    // self and other always need to be pushed but they should be params
    PUSH_GREF(self);
    PUSH_GREF(other);
    // pushing this improves frame efficiency by around 34% on Ryzen
    PUSH_GFLOAT(force_retouch);
    // time needs to be pushed for accuracy
    PUSH_GFLOAT(time);

    if (fnum == pr_global_struct->PutClientInServer) {
        PUSH_GFLOAT(parm1);
        PUSH_GFLOAT(parm2);
        PUSH_GFLOAT(parm3);
        PUSH_GFLOAT(parm4);
        PUSH_GFLOAT(parm5);
        PUSH_GFLOAT(parm6);
        PUSH_GFLOAT(parm7);
        PUSH_GFLOAT(parm8);
        PUSH_GFLOAT(parm9);
    }

    if (lua_pcall(L, 0, 0, 0) != LUA_OK)
        SV_Error((char *)lua_tostring(L, -1));

    if (fnum == pr_global_struct->SetChangeParms || fnum == pr_global_struct->SetNewParms) {
        GET_GFLOAT(parm1);
        GET_GFLOAT(parm2);
        GET_GFLOAT(parm3);
        GET_GFLOAT(parm4);
        GET_GFLOAT(parm5);
        GET_GFLOAT(parm6);
        GET_GFLOAT(parm7);
        GET_GFLOAT(parm8);
        GET_GFLOAT(parm9);
    }
}

edict_t *EDICT_NUM(int n)
{
    if (n < 0 || n >= MAX_EDICTS)
        SV_Error("EDICT_NUM: bad number %i", n);
    return (edict_t *) ((byte *) sv.edicts + (n) * pr_edict_size);
}

int NUM_FOR_EDICT(edict_t * e)
{
    int b;

    b = (byte *) e - (byte *) sv.edicts;
    b = b / pr_edict_size;

    if (b < 0 || b >= sv.num_edicts)
        SV_Error("NUM_FOR_EDICT: bad pointer");
    return b;
}

char *PR_GetString(int num)
{
    static char buf[256];

    if (num == 0)
        return "";

    lua_rawgeti(L, LUA_REGISTRYINDEX, num);

    snprintf(buf, sizeof(buf), "%s", luaL_checkstring(L, -1));

    lua_pop(L, 1);

    //Con_Printf("PR_GetString(%d) -> '%s'\n", num, buf);

    return buf;
}

int PR_SetString(char *s)
{
    lua_pushstring(L, s);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

// XXX: these are *never* freed at this point
char *PR_StrDup(const char *in)
{
    char *out;

    out = Z_Malloc(strlen(in) + 1);
    strcpy(out, in);

    return out;
}

edict_t *PROG_TO_EDICT(int ref)
{
    edict_t **e;

    if (ref == 0)
        return NULL;

    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    e = luaL_checkudata(L, -1, "edict_t");

    lua_pop(L, 1);

    return *e;
}
