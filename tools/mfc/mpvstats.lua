-- mpvstats.lua - G17 (notes/g17-dxva-mfc.md): log mpv's own playback counters once a second into its log file
-- (mpv --script=C:\S10eTmp\mfc\mpvstats.lua --log-file=...): decoder drops, VO drops, delayed frames, the video
-- output's estimated frame rate and the measured display rate / vsync jitter. Lines are tagged "[mpvstats]".
local function p(name)
    local v = mp.get_property_native(name)
    if v == nil then return "-" end
    if type(v) == "number" and v ~= math.floor(v) then return string.format("%.2f", v) end
    return tostring(v)
end

mp.add_periodic_timer(1, function()
    mp.msg.info(string.format("t=%s decdrop=%s vodrop=%s delayed=%s vf-fps=%s disp-fps=%s jitter=%s",
        p("time-pos"), p("frame-drop-count"), p("vo-drop-frame-count"), p("vo-delayed-frame-count"),
        p("estimated-vf-fps"), p("estimated-display-fps"), p("vsync-jitter")))
end)
