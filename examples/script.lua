-- In-game test for the http mod. Enable the "StormFabric HTTP Test" addon in a new world
-- (install.bat copies it). Works without the mod: `http` is nil.
--
-- Chat commands:
--   ?http test                     runs every check below and prints TEST OK / TEST FAILED
--   ?http get <url> [options]      e.g. ?http get https://httpbin.org/get timeout=5
--   ?http post <url> <text>        sends <text> (JSON if it starts with { or [)
--   ?http weather <city>           real temperature from wttr.in (http.field)
--   ?http discord <text>           posts <text> to DISCORD_WEBHOOK below (json_encode)
--   ?http pending | version

-- Paste your Discord webhook URL here (a webhook URL is a password: keep it private).
-- It is too long to be typed in the game chat, which cuts long messages.
local DISCORD_WEBHOOK = ""

local handlers = {}   -- id -> function(kind, a, b, headers)
local failures, checks = 0, 0
local failed = {}

local function say(text, peer)
    server.announce("[http]", text, peer or -1)
end

local function short(text)
    text = tostring(text)
    if #text > 180 then return text:sub(1, 180) .. "... (" .. #text .. " bytes)" end
    return text
end

local function check(ok, what)
    checks = checks + 1
    if not ok then
        failures = failures + 1
        failed[#failed + 1] = what
    end
    say((ok and "ok: " or "FAILED: ") .. what)
end

-- Starts a request; `done(kind, a, b, headers)` runs when http.poll returns it.
local function send(id, err, done, peer)
    if not id then
        say("request refused: " .. tostring(err), peer)
        return
    end
    handlers[id] = done
end

function onCreate(is_world_create)
    if http then say("http " .. http.version() .. " loaded") else say("http mod not available") end
end

function onTick(game_ticks)
    if not http then return end
    while true do
        local id, kind, a, b, headers = http.poll()
        if not id then break end
        local done = handlers[id]
        handlers[id] = nil
        if done then done(kind, a, b, headers)
        elseif kind == "response" then say("#" .. id .. " HTTP " .. a .. ": " .. short(b))
        else say("#" .. id .. " error: " .. tostring(a)) end
    end
end

local function run_tests(peer)
    failures, checks, failed = 0, 0, {}
    local remaining = 3
    local function finished()
        remaining = remaining - 1
        if remaining == 0 then
            say((failures == 0 and "TEST OK" or "TEST FAILED") .. " (" .. (checks - failures) .. "/" .. checks .. ")")
            for _, what in ipairs(failed) do say("  failed: " .. what) end
        end
    end

    -- Pure functions (no network).
    local t, terr = http.json_decode('{"a":[1,2,{"b":"c"}],"n":2.5,"t":true}')
    check(t and t.a and t.a[2] == 2 and t.a[3].b == "c" and t.n == 2.5 and t.t == true, "json_decode " .. tostring(terr or ""))
    local text, eerr = http.json_encode({name = "storm", list = {1, 2, 3}})
    check(text == '{"list":[1,2,3],"name":"storm"}' or text == '{"name":"storm","list":[1,2,3]}',
          "json_encode: " .. tostring(text or eerr))
    check(http.field('{"x":{"y":[5,6]}}', "x.y.2") == 6, "field")
    check(http.urlencode("a b&c") == "a%20b%26c", "urlencode")

    -- HTTPS GET with a header, read back through JSON.
    local id, err = http.get("https://httpbin.org/get?from=stormworks", "header=X-Test: sf;timeout=20")
    send(id, err, function(kind, status, body, headers)
        check(kind == "response" and status == 200, "GET https status " .. tostring(status))
        check(http.field(body, "args.from") == "stormworks", "GET query echoed")
        check(http.field(body, "headers.X-Test") == "sf", "GET header sent")
        check(headers and headers["content-type"] ~= nil, "response headers table")
        finished()
    end, peer)

    -- JSON POST.
    local payload = http.json_encode({hello = "world", n = 42})
    id, err = http.post("https://httpbin.org/post", payload, "timeout=20")
    send(id, err, function(kind, status, body)
        check(kind == "response" and status == 200, "POST status " .. tostring(status))
        check(http.field(body, "json.n") == 42, "POST json echoed")
        finished()
    end, peer)

    -- A 404 is a response, a bad host is an error.
    id, err = http.get("https://does-not-exist.invalid/", "timeout=10")
    send(id, err, function(kind, message)
        check(kind == "error", "network error reported: " .. short(message))
        finished()
    end, peer)
end

function onCustomCommand(full_message, peer_id, is_admin, is_auth, command, sub, a1, a2)
    if command ~= "?http" then return end
    if not http then
        say("http mod not available", peer_id)
        return
    end
    if sub == "test" then
        run_tests(peer_id)
    elseif sub == "get" and a1 then
        local id, err = http.get(a1, a2)
        send(id, err, nil, peer_id)
        if id then say("#" .. id .. " GET " .. a1, peer_id) end
    elseif sub == "post" and a1 then
        local body = full_message:match("^%S+%s+%S+%s+%S+%s+(.*)$") or ""
        local id, err = http.post(a1, body)
        send(id, err, nil, peer_id)
        if id then say("#" .. id .. " POST " .. a1, peer_id) end
    elseif sub == "weather" and a1 then
        local id, err = http.get("https://wttr.in/" .. http.urlencode(a1) .. "?format=j1", "timeout=20")
        send(id, err, function(kind, status, body)
            if kind ~= "response" or status ~= 200 then
                say("weather failed: " .. tostring(status), peer_id)
                return
            end
            local temp = http.field(body, "current_condition.1.temp_C")
            local desc = http.field(body, "current_condition.1.weatherDesc.1.value")
            say(a1 .. ": " .. tostring(temp) .. " C, " .. tostring(desc), peer_id)
        end, peer_id)
    elseif sub == "discord" then
        if DISCORD_WEBHOOK == "" then
            say("put your webhook URL in DISCORD_WEBHOOK at the top of script.lua, then reload the world", peer_id)
            return
        end
        local text = full_message:match("^%S+%s+%S+%s+(.*)$") or "hello from Stormworks"
        local id, err = http.post(DISCORD_WEBHOOK, http.json_encode({content = text}))
        send(id, err, function(kind, status, body)
            -- Discord answers 204 No Content on success, and explains a failure in the body.
            if kind == "response" and status < 300 then say("discord: HTTP " .. status .. " (sent)", peer_id)
            elseif kind == "response" then say("discord: HTTP " .. status .. " " .. short(body), peer_id)
            else say("discord error: " .. tostring(status), peer_id) end
        end, peer_id)
    elseif sub == "pending" then
        say("pending: " .. http.pending(), peer_id)
    elseif sub == "version" then
        say(http.version(), peer_id)
    else
        say("usage: ?http test | get <url> [options] | post <url> <text> | weather <city> | discord <text> | pending | version", peer_id)
    end
end
