#!/usr/bin/haserl
<%in p/common.cgi %>
<%
# Machino camera/media page for the OpenIPC WebUI. It keeps the stock WebUI
# (layout, navigation, System/Network pages, login) completely untouched and
# only replaces the Majestic-specific camera part: when Machino is the active
# streamer it talks to Machino's REST API on 127.0.0.1:8080 SERVER-SIDE (busybox
# wget, GET + the M10 majestic-webui POST), so no second login and no CORS. It
# also carries the machino/majestic switch, because this is the page you use to
# switch back (then Machino has no API at all).
pagename="machino"
page_title="Media service"

STREAMERCTL=/usr/sbin/streamerctl
API="http://127.0.0.1:8080/api/v1"
result=""
result_class="warning"

api_get() { wget -q -O- -T 3 "$API/$1" 2>/dev/null; }
# JSON field pluckers (busybox grep/sed): first "key":number / "key":"string".
jnum() { printf '%s' "$1" | grep -o "\"$2\":[0-9][0-9.]*" | head -1 | sed 's/.*://'; }
jstr() { printf '%s' "$1" | grep -o "\"$2\":\"[^\"]*\"" | head -1 | sed 's/^[^:]*:"//; s/"$//'; }
digits() { printf '%s' "$1" | tr -cd '0-9'; }

# ---- POST: either switch the streamer, or apply Machino settings ------------
if [ "$REQUEST_METHOD" = "POST" ] && [ -n "$POST_streamer" ]; then
	case "$POST_streamer" in
		machino|majestic)
			out=$("$STREAMERCTL" set "$POST_streamer" 2>&1)
			if [ $? -eq 0 ]; then
				result="Active media service is now: $POST_streamer"; result_class="success"
			else
				result="Switching to $POST_streamer failed; rolled back. Details: $out"; result_class="danger"
			fi ;;
		*) result="Unknown media service."; result_class="danger" ;;
	esac
elif [ "$REQUEST_METHOD" = "POST" ] && [ -n "$POST_apply" ]; then
	# Native validation stays in Machino; here we only forward the WebUI form
	# through the M10 majestic-webui-compat POST. Sanitise to digits first.
	f=$(digits "$POST_fps"); b=$(digits "$POST_bitrate"); g=$(digits "$POST_gop"); s=$(digits "$POST_sensor_fps")
	body="{\"video0\":{\"fps\":${f:-0},\"bitrate_kbps\":${b:-0},\"gop\":${g:-0}},\"sensor\":{\"fps\":${s:-0}}}"
	resp=$(wget -q -O- -T 5 --header='Content-Type: application/json' --post-data="$body" "$API/config" 2>/dev/null)
	if printf '%s' "$resp" | grep -q '"ok":true'; then
		result="Machino settings applied."; result_class="success"
	else
		msg=$(jstr "$resp" message); [ -n "$msg" ] || msg="$resp"
		result="Applying settings failed: $msg"; result_class="danger"
	fi
fi

status_raw=$("$STREAMERCTL" status 2>/dev/null)
selected=$(echo "$status_raw" | sed -n 's/^selected:[[:space:]]*//p')
running=$(echo "$status_raw"  | sed -n 's/^running:[[:space:]]*//p')
port80=$(echo "$status_raw"   | sed -n 's/^port 80:[[:space:]]*//p')
[ -n "$selected" ] || selected="majestic"

# ---- live Machino data (only when Machino is actually running) --------------
mach_up=0
if [ "$running" = "machino" ]; then
	CFG=$(api_get config.json)
	ST=$(api_get state)
	TEL=$(api_get telemetry)
	if [ -n "$CFG" ]; then
		mach_up=1
		V0=$(printf '%s' "$CFG" | grep -o '"video0":{[^}]*}')
		SEN=$(printf '%s' "$CFG" | grep -o '"sensor":{[^}]*}')
		c_fps=$(jnum "$V0" fps); c_br=$(jnum "$V0" bitrate_kbps); c_gop=$(jnum "$V0" gop)
		c_w=$(jnum "$V0" width); c_h=$(jnum "$V0" height); c_sfps=$(jnum "$SEN" fps)
		s_life=$(jstr "$ST" lifecycle)
		MED=$(printf '%s' "$TEL" | grep -o '"media":{[^}]*}')
		PROC=$(printf '%s' "$TEL" | grep -o '"process":{[^}]*}')
		t_efps=$(jnum "$MED" encoded_fps); t_ebr=$(jnum "$MED" bitrate_kbps)
		t_rss=$(jnum "$PROC" rss_kb); t_thr=$(jnum "$PROC" threads)
	fi
fi
cam_ip=$(printf '%s' "${HTTP_HOST:-camera}" | sed 's/:.*//')
%>
<%in p/header.cgi %>

<div class="row">
	<div class="col-lg-8">
		<% [ -n "$result" ] && printf '<div class="alert alert-%s">%s</div>' "$result_class" "$(attr_escape "$result")" %>

		<% if [ "$running" = "machino" ] && [ "$mach_up" = "1" ]; then %>
		<% card_head "Camera (Machino)" %>
		<dl class="row mb-2">
			<dt class="col-sm-4">State</dt><dd class="col-sm-8"><% esc "${s_life:-?}" %></dd>
			<dt class="col-sm-4">Resolution</dt><dd class="col-sm-8"><% esc "${c_w:-?}" %>×<% esc "${c_h:-?}" %></dd>
			<dt class="col-sm-4">Encoded now</dt><dd class="col-sm-8"><% esc "${t_efps:-–}" %> fps · <% esc "${t_ebr:-–}" %> kbit/s</dd>
			<dt class="col-sm-4">Process</dt><dd class="col-sm-8">RSS <% esc "${t_rss:-?}" %> kB · <% esc "${t_thr:-?}" %> threads</dd>
			<dt class="col-sm-4">RTSP</dt><dd class="col-sm-8"><code>rtsp://<% esc "$cam_ip" %>:554/ch0</code></dd>
		</dl>

		<form method="post" action="machino.cgi">
			<div class="row">
				<div class="col-sm-6 mb-3">
					<label class="form-label" for="fps">Stream frame rate (fps)</label>
					<input class="form-control" type="number" id="fps" name="fps" min="1" max="120" value="<% esc "${c_fps}" %>">
				</div>
				<div class="col-sm-6 mb-3">
					<label class="form-label" for="bitrate">Bitrate (kbit/s)</label>
					<input class="form-control" type="number" id="bitrate" name="bitrate" min="64" max="100000" value="<% esc "${c_br}" %>">
				</div>
				<div class="col-sm-6 mb-3">
					<label class="form-label" for="gop">Keyframe interval (frames)</label>
					<input class="form-control" type="number" id="gop" name="gop" min="1" max="1000" value="<% esc "${c_gop}" %>">
				</div>
				<div class="col-sm-6 mb-3">
					<label class="form-label" for="sensor_fps">Sensor frame rate (fps)</label>
					<input class="form-control" type="number" id="sensor_fps" name="sensor_fps" min="1" max="120" value="<% esc "${c_sfps}" %>">
				</div>
			</div>
			<button class="btn btn-primary" type="submit" name="apply" value="1">Apply</button>
		</form>
		<p class="text-secondary mt-2 mb-0"><small>Live video plays over RTSP (open the URL above in VLC). In-browser
		preview needs an MJPEG/snapshot endpoint, which Machino does not expose yet.</small></p>
		<% elif [ "$running" = "machino" ]; then %>
		<% card_head "Camera (Machino)" %>
		<div class="alert alert-warning">Machino is selected but its API on 127.0.0.1:8080 did not answer. Try reloading.</div>
		<% else %>
		<% card_head "Camera (Machino)" %>
		<p class="text-secondary">Majestic is the active media service. Switch to Machino below to control it here.</p>
		<% fi %>

		<% card_head "Active media service" %>
		<p class="text-secondary">
			Machino and Majestic drive the same camera hardware, so only one runs at
			a time. Switching takes a few seconds: the running service is stopped
			before the other starts. If the new one fails to come up, the camera
			returns to the previous one automatically.
		</p>
		<form method="post" action="machino.cgi">
			<div class="mb-3">
				<label class="form-label" for="streamer">Media service</label>
				<select class="form-select" id="streamer" name="streamer">
					<option value="majestic"<% [ "$selected" = "majestic" ] && printf ' selected' %>>Majestic</option>
					<option value="machino"<% [ "$selected" = "machino" ] && printf ' selected' %>>Machino</option>
				</select>
			</div>
			<% button_submit %>
		</form>

		<hr>
		<dl class="row mb-0">
			<dt class="col-sm-4">Selected at boot</dt><dd class="col-sm-8"><% esc "$selected" %></dd>
			<dt class="col-sm-4">Currently running</dt><dd class="col-sm-8"><% esc "$running" %></dd>
			<dt class="col-sm-4">Serving this page</dt><dd class="col-sm-8"><% esc "$port80" %></dd>
		</dl>
	</div>
</div>

<%in p/footer.cgi %>
