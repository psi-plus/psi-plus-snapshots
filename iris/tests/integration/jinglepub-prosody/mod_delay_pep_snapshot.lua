local st = require "util.stanza"
local filters = require "util.filters"

local target_node = module:get_option_string("delay_pep_snapshot_node", "urn:xmpp:jinglepub:ci")
local delay = module:get_option_number("delay_pep_snapshot_seconds", 1.5)
local suppress_user = module:get_option_string("delay_pep_snapshot_suppress_user", "user02")
local suppress_resource = module:get_option_string("delay_pep_snapshot_suppress_resource", "publisher")
local suppress_jid = suppress_user .. "@" .. module.host .. "/" .. suppress_resource

local function is_target_pep_event(stanza)
    if not stanza or stanza.name ~= "message" or stanza.attr.type ~= "headline" or stanza.attr.to ~= suppress_jid then
        return false
    end
    local pubsub_event = stanza:get_child("event", "http://jabber.org/protocol/pubsub#event")
    local items = pubsub_event and pubsub_event:get_child("items")
    return items and items.attr.node == target_node
end

-- Return an authoritative empty snapshot, but only after a delay. This models
-- a PubSub items query which raced with a subsequent publish: the snapshot was
-- taken before the item existed, while its IQ result arrives after publish ACK.
module:hook("pre-iq/bare", function (event)
    local stanza = event.stanza
    if stanza.attr.type ~= "get" then
        return
    end

    local pubsub = stanza:get_child("pubsub", "http://jabber.org/protocol/pubsub")
    local items = pubsub and pubsub:get_child("items")
    if not items or items.attr.node ~= target_node then
        return
    end

    local reply = st.reply(stanza)
        :tag("pubsub", { xmlns = "http://jabber.org/protocol/pubsub" })
            :tag("items", { node = target_node }):up()
        :up()
    local origin = event.origin
    module:log("debug", "Delaying empty PEP snapshot for %s by %.2fs", target_node, delay)
    module:add_timer(delay, function ()
        if origin and origin.send then
            origin.send(reply)
        end
    end)
    return true
end, 1000)

-- mod_pep can write the owner's self-notification directly to the c2s session,
-- bypassing normal message routing hooks. Install an outgoing stanza filter so
-- the race fixture can model servers which do not deliver that self event.
local function filter_session(session)
    filters.add_filter(session, "stanzas/out", function (stanza)
        if is_target_pep_event(stanza) then
            module:log("debug", "Suppressing self-PEP event for %s on %s", suppress_jid, target_node)
            return nil
        end
        return stanza
    end, 1000)
end

function module.load()
    filters.add_filter_hook(filter_session)
end

function module.unload()
    filters.remove_filter_hook(filter_session)
end
