-- frame function wrapper
function ffunc(frame, think, callback)
    return function()
        self.frame = frame
        self.nextthink = time + 0.1
        self.think = think
        if callback then
            callback()
        end
    end
end

require "defs"
require "subs"
require "combat"
require "items"
require "weapons"
require "world"
require "client"
require "spectate"
require "player"
require "doors"
require "buttons"
require "triggers"
require "plats"
require "misc"
require "server"

-- client.qc
function player_run() end
function ClientObituary(a,b) end

function PlayerPreThink()
    -- dprint("PlayerPreThink()\n")
end

function PlayerPostThink()
    -- dprint("PlayerPostThink()\n")
end

function ClientConnect()
    dprint ("ClientConnect()\n")
end

function ClientDisconnect()
    dprint ("ClientDisconnect()\n");
end

--[[
function PutClientInServer()
    dprint("PutClientInServer()\n");

    local spawn_spot = find (world, "classname", "info_player_start")

    self.classname = "player"
    self.health = 100
    self.max_health = self.health
    self.takedamage = DAMAGE_AIM
    self.solid = SOLID_SLIDEBOX
    self.movetype = MOVETYPE_WALK
    self.flags = FL_CLIENT

    self.origin = spawn_spot.origin + vec3(0, 0, 1)
    self.angles = spawn_spot.angles
    self.fixangle = TRUE
    self.th_die = function() end

    setmodel (self, "progs/player.mdl")
    setsize (self, VEC_HULL_MIN, VEC_HULL_MAX)

    self.view_ofs = vec3(0, 0, 22)
    self.velocity = vec3(0, 0, 0)
end
--]]
