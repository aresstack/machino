#!/usr/bin/haserl
<%in p/common.cgi %>
<%
# Machino: select the active media owner.
#
# This page must work no matter which streamer is running, because it is the
# page you use to switch back. It therefore talks only to /usr/sbin/streamerctl
# and never to a Machino API: when Majestic is active, Machino is stopped and
# has no API at all.
pagename="machino"
page_title="Media service"

STREAMERCTL=/usr/sbin/streamerctl
result=""
result_class="warning"

if [ "$REQUEST_METHOD" = "POST" ] && [ -n "$POST_streamer" ]; then
	case "$POST_streamer" in
		machino|majestic)
			out=$("$STREAMERCTL" set "$POST_streamer" 2>&1)
			if [ $? -eq 0 ]; then
				result="Active media service is now: $POST_streamer"
				result_class="success"
			else
				result="Switching to $POST_streamer failed; the camera was rolled back. Details: $out"
				result_class="danger"
			fi
			;;
		*)
			result="Unknown media service."
			result_class="danger"
			;;
	esac
fi

status_raw=$("$STREAMERCTL" status 2>/dev/null)
selected=$(echo "$status_raw" | sed -n 's/^selected:[[:space:]]*//p')
running=$(echo "$status_raw"  | sed -n 's/^running:[[:space:]]*//p')
port80=$(echo "$status_raw"   | sed -n 's/^port 80:[[:space:]]*//p')
[ -n "$selected" ] || selected="majestic"
%>
<%in p/header.cgi %>

<div class="row">
	<div class="col-lg-8">
		<% [ -n "$result" ] && printf '<div class="alert alert-%s">%s</div>' "$result_class" "$(attr_escape "$result")" %>

		<% card_head "Active media service" %>
		<p class="text-secondary">
			Machino and Majestic both drive the same camera hardware, so only one of
			them may run at a time. Switching takes a few seconds: the running
			service is stopped completely before the other one is started. If the
			new one does not come up, the camera automatically returns to the
			previous one.
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
			<dt class="col-sm-4">Selected at boot</dt>
			<dd class="col-sm-8"><% esc "$selected" %></dd>
			<dt class="col-sm-4">Currently running</dt>
			<dd class="col-sm-8"><% esc "$running" %></dd>
			<dt class="col-sm-4">Serving this page</dt>
			<dd class="col-sm-8"><% esc "$port80" %></dd>
		</dl>
		<% [ "$port80" = "none" ] && printf '<div class="alert alert-danger mt-3">Nothing is serving port 80. Use <code>streamerctl set majestic</code> over SSH.</div>' %>
	</div>
</div>

<%in p/footer.cgi %>
