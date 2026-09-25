#!/usr/bin/haserl
<%in p/common.cgi %>
<%
# DynDNS: eine machino-eigene OpenIPC-Seite (haserl, wie die Stock-Dienste
# wireguard.cgi/vtun.cgi). KEIN machinod-Backend -- der Updater
# /usr/sbin/machino-dyndns laeuft eigenstaendig. Die Seite schreibt die Config
# und stoesst den Updater an; das Speichern parst haserl selbst (POST_*), der
# CGI-Shim ist dafuer nicht noetig.
CONF=/etc/machino/dyndns.conf
DDNS=/usr/sbin/machino-dyndns

dd_get() {
	[ -f "$CONF" ] || return
	sed -n "s/^$1=//p" "$CONF" | tail -1
}

if [ "$REQUEST_METHOD" = "POST" ]; then
	if [ "$POST_action" = "reset" ]; then
		"$DDNS" stop >/dev/null 2>&1
		rm -f "$CONF"
		redirect_to "$SCRIPT_NAME" "danger" "DynDNS is off"
	else
		mkdir -p /etc/machino
		_en=off; [ "$POST_enabled" = "on" ] && _en=true
		_iv=$(t_value "POST_interval"); [ -n "$_iv" ] || _iv=300
		# Zeilenweise, Werte unverändert (die Update-URL trägt Token und ?=).
		{
			printf 'enabled=%s\n' "$_en"
			printf 'url=%s\n' "$(t_value "POST_url")"
			printf 'interval=%s\n' "$_iv"
		} > "$CONF"
		chmod 0600 "$CONF"
		"$DDNS" restart >/dev/null 2>&1
		redirect_to "$SCRIPT_NAME" "success" "DynDNS saved"
	fi
fi

dd_enabled=$(dd_get enabled)
dd_url=$(dd_get url)
dd_interval=$(dd_get interval)
[ -n "$dd_interval" ] || dd_interval=300
dd_status=$($DDNS status 2>/dev/null)
%>
<%in p/header.cgi %>

<div id="mch">
<div class="row g-4">

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Dynamic DNS</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Keeps a hostname pointed at this camera's current public IP by
	  calling your provider's update URL on a timer. Most providers (IONOS, DuckDNS,
	  dynv6, No-IP) take a single token URL and read the address from the request
	  itself &mdash; paste that URL below.</p>
	<dl class="row mb-0">
		<dt class="col-5">Updater</dt><dd class="col-7"><%= $(echo "$dd_status" | sed -n 's/^running: //p') %></dd>
		<dt class="col-5">Enabled</dt><dd class="col-7"><%= $(echo "$dd_status" | sed -n 's/^enabled: //p') %></dd>
		<dt class="col-5">Last result</dt><dd class="col-7 text-break"><%= $(echo "$dd_status" | sed -n 's/^last: *//p') %></dd>
	</dl>
	<p class="mj-card-note">The last line is <code>epoch ok|fail http ip message</code> as the
	  updater recorded it.</p>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Provider</h3><span class="mj-live-rule"></span></div>
	<form action="<%= $SCRIPT_NAME %>" method="post">
		<label class="d-block mb-2"><input type="checkbox" name="enabled" style="width:auto"<% [ "$dd_enabled" = "true" ] && echo " checked" %>> Enabled</label>
		<label class="form-label" for="dd-url">Update URL</label>
		<input class="form-control mb-2" id="dd-url" name="url" placeholder="https://&hellip;/update?token=&hellip;" value="<%= $dd_url %>">
		<label class="form-label" for="dd-iv">Interval (seconds, min 60)</label>
		<input class="form-control mb-3" id="dd-iv" name="interval" value="<%= $dd_interval %>">
		<button class="btn btn-sm btn-primary" type="submit">Save</button>
		<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="reset">Turn off</button>
	</form>
	<p class="mj-card-note">Saving restarts the updater and runs one update immediately. The URL
	  is stored on the camera in <code>/etc/machino/dyndns.conf</code> (mode 0600); its token is a
	  secret, treat it like a password.</p>
</div></div></div>

</div>
</div>

<%in p/footer.cgi %>
