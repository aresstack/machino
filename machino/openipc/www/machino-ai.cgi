#!/usr/bin/haserl
<%in p/common.cgi %>
<%
# KI: eine machino-eigene OpenIPC-Seite (haserl, wie machino-dyndns.cgi).
# KEIN machinod-Backend -- die Seite verwaltet genau das, was machinod NICHT
# besitzt: die NNA-Voraussetzungen (Bootarg, Treiber, Geraeteknoten), die
# Modelldateien unter /etc/machino/models und den Platz dafuer. Detector,
# inference_fps und ai.enabled bleiben bei machinod (ai-Sektion der WebUI /
# API) -- zwei Besitzer derselben Einstellung waren der Architekturbefund
# vom WLAN (2026-09-25), der hier nicht wiederholt wird.
MODELDIR=/etc/machino/models
BACKUP=/etc/machino/backup

if [ "$REQUEST_METHOD" = "POST" ]; then
	case "$POST_action" in
	purge-backup)
		# Das majestic-Backup sichert den Uninstall-Rueckweg -- auf dem
		# T40NN einen zu einem nachweislich defekten majestic (ABI-Mismatch,
		# kein Videopfad). Der echte Rueckweg ist ein OpenIPC-Reflash, der
		# Bootloader bleibt dabei unangetastet. Loeschen ist hier also kein
		# Verlust, sondern ~2,9 MB Platz fuer Modelle. Nur auf Knopfdruck,
		# nie still: wer auf einem Board mit heilem majestic sitzt, soll
		# diese Entscheidung sehen und selbst treffen.
		if [ -d "$BACKUP" ]; then
			rm -rf "$BACKUP"
			redirect_to "$SCRIPT_NAME" "success" "Backup deleted"
		else
			redirect_to "$SCRIPT_NAME" "danger" "No backup present"
		fi
		;;
	del-model)
		# Nur nackte Dateinamen im Modellordner. Alles mit / oder .. ist
		# kein Modellname, sondern ein Pfadversuch -- und wird keiner.
		_n=$(t_value "POST_name")
		case "$_n" in
		''|*/*|*..*) redirect_to "$SCRIPT_NAME" "danger" "Bad model name" ;;
		esac
		if [ -f "$MODELDIR/$_n" ]; then
			rm -f "$MODELDIR/$_n"
			redirect_to "$SCRIPT_NAME" "success" "Model deleted"
		else
			redirect_to "$SCRIPT_NAME" "danger" "No such model"
		fi
		;;
	esac
fi

# Voraussetzungen, jede einzeln und ehrlich: gruen erst, wenn sie DA ist.
nna_dev=no;   [ -c /dev/soc-nna ] && nna_dev=yes
nna_ko=no;    grep -q '^soc_nna' /proc/modules 2>/dev/null && nna_ko=yes
nna_nmem=no;  grep -q 'nmem=' /proc/cmdline 2>/dev/null && nna_nmem=yes
nna_helper=no; [ -x /usr/sbin/machino-nna ] && nna_helper=yes
model_count=0
[ -d "$MODELDIR" ] && model_count=$(ls -1 "$MODELDIR" 2>/dev/null | wc -l)

backup_kb=0
[ -d "$BACKUP" ] && backup_kb=$(du -ks "$BACKUP" 2>/dev/null | cut -f1)
free_kb=$(df -k / 2>/dev/null | awk 'NR==2{print $4}')

# Wohin ai.model_path zeigt -- nur ANZEIGE. Geschrieben wird der Wert ueber
# machinods API/WebUI; diese Seite liest ihn, damit sichtbar ist, welches
# der Dateien unten tatsaechlich gemeint ist.
model_cfg=$(sed -n 's/^ai\.model_path=//p' /etc/machino/machino.conf 2>/dev/null | tail -1)
%>
<%in p/header.cgi %>

<div id="mch">
<div class="row g-4">

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Neural accelerator</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">The T40 has a hardware NN accelerator (Ingenic NNA). Person and
	  object detection run there &mdash; not on the CPU. Everything below must be green
	  before a detector other than <code>motion</code> can work.</p>
	<dl class="row mb-0">
		<dt class="col-7">NNA memory reserved (nmem)</dt><dd class="col-5"><%= $nna_nmem %></dd>
		<dt class="col-7">Driver loaded (soc-nna)</dt><dd class="col-5"><%= $nna_ko %></dd>
		<dt class="col-7">Device node /dev/soc-nna</dt><dd class="col-5"><%= $nna_dev %></dd>
		<dt class="col-7">Inference helper installed</dt><dd class="col-5"><%= $nna_helper %></dd>
		<dt class="col-7">Models on camera</dt><dd class="col-5"><%= $model_count %></dd>
	</dl>
	<p class="mj-card-note">The nmem boot parameter is set from the Cam-Tool (it changes
	  U-Boot arguments and is applied on the next boot). Driver and helper come with the
	  AI payload. Detector choice and inference rate live in the camera's AI settings,
	  not here &mdash; this page owns the platform underneath.</p>
</div></div></div>

<div class="col-12 col-lg-6"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Storage</h3><span class="mj-live-rule"></span></div>
	<dl class="row mb-0">
		<dt class="col-7">Free space on /</dt><dd class="col-5"><%= $free_kb %> kB</dd>
		<dt class="col-7">majestic backup</dt><dd class="col-5"><% if [ "$backup_kb" -gt 0 ]; then echo "${backup_kb} kB"; else echo "none"; fi %></dd>
	</dl>
	<% if [ "$backup_kb" -gt 0 ]; then %>
	<p class="mj-card-note">The backup exists so uninstalling Machino can restore majestic.
	  On this board the backed-up majestic is <b>broken</b> (ABI mismatch, no video path) &mdash;
	  the real way back is reflashing OpenIPC, which leaves the bootloader untouched.
	  Deleting the backup frees space for AI models. Uninstall will then simply not
	  restore a media daemon.</p>
	<form action="<%= $SCRIPT_NAME %>" method="post"
	      onsubmit="return confirm('Delete the majestic backup? Uninstalling Machino will no longer restore majestic; the way back is reflashing OpenIPC.')">
		<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="purge-backup"
		        title="Frees ~<%= $backup_kb %> kB. The way back to stock majestic is reflashing OpenIPC.">Delete backup</button>
	</form>
	<% fi %>
</div></div></div>

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Models</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Model files live in <code>/etc/machino/models</code>. They are
	  quantized Magik models (TransformKit output for the T40). Copy them onto the camera
	  with the Cam-Tool or scp; the AI settings' <code>model path</code> then points at one
	  of them<% [ -n "$model_cfg" ] && echo " (currently: <code>$model_cfg</code>)" %>.</p>
	<% if [ "$model_count" -gt 0 ]; then %>
	<table class="table table-sm mb-0"><thead><tr><th>File</th><th>Size</th><th></th></tr></thead><tbody>
	<% for f in "$MODELDIR"/*; do [ -f "$f" ] || continue; _b=$(basename "$f"); _s=$(du -k "$f" 2>/dev/null | cut -f1) %>
		<tr><td class="text-break"><%= $_b %></td><td><%= $_s %> kB</td>
		<td><form action="<%= $SCRIPT_NAME %>" method="post" class="d-inline"
		          onsubmit="return confirm('Delete this model?')">
			<input type="hidden" name="name" value="<%= $_b %>">
			<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="del-model">Delete</button>
		</form></td></tr>
	<% done %>
	</tbody></table>
	<% else %>
	<p class="mj-card-note">No models on the camera yet.</p>
	<% fi %>
</div></div></div>

</div>
</div>

<%in p/footer.cgi %>
