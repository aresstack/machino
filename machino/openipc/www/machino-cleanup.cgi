#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="Storage" %>
<%
# Storage / Speicher-Manager: eine machino-eigene OpenIPC-Seite (haserl, wie
# machino-ai.cgi). Aufgebaut nach SPEICHERBEREICHEN -- wie "Dieser PC" mit
# Laufwerken:
#
#   Overlay (mtd4, jffs2, beschreibbar)  -> HIER wird Platz frei. Zeigt nur
#       system-irrelevante Dateien (Backups, Staging, alte Payloads, KI-Modelle)
#       und macht sie herunterladbar (Download-Knopf) und einzeln loeschbar.
#   Firmware/ROM (mtd3, squashfs, read-only) -> NUR Ansicht. Einzelne Dateien
#       lassen sich nicht loeschen (ein overlayfs-Whiteout KOSTET Overlay-Platz
#       und gibt kein ROM frei; ROM aendert nur ein ganzer Reflash). majestic
#       bekommt hier einen "deaktivieren"-Knopf (Whiteout), keinen Loeschknopf.
#   Extern (SD/USB) -> falls vorhanden; sonst der Hinweis, dass Modelle dorthin
#       ausgelagert werden koennten.
#   Partitionen -> read-only Info (/proc/mtd). KEIN Repartitionieren: das
#       schreibt die U-Boot-Env, brickt bei einem Fehler und steht auf der
#       No-Go-Liste (docs/pending-physical.md). Hier lohnt es zudem nicht, weil
#       das squashfs seine 5-MB-Partition schon zu ~92% fuellt.
#
# KEIN machinod-Backend: die Seite verwaltet Dateien, die machinod nicht
# besitzt. Loeschen/Download laufen als CGI (root) unter busybox httpd :85.
# Keine OpenIPC-Datei wird veraendert; uninstall entfernt diese Seite.

WO=/overlay/root/etc/init.d/S95majestic   # overlayfs-Whiteout = majestic aus

# Ist ein Pfad ein zulaessiges Aufraeum-Ziel im OVERLAY? Serverseitig streng
# geprueft, damit ein Pfad aus dem Netz NIE zu einer beliebigen Datei wird:
# kein "..", nur regulaere Dateien, nur die bekannten Fundstellen. Das laufende
# /usr/bin/machino (ohne Suffix), webui-upstream.tgz und aktive Binaries fallen
# durch.
is_reclaimable() {
	_p=$1
	case "$_p" in *..*) return 1 ;; esac
	[ -f "$_p" ] || return 1
	case "$_p" in
		/etc/machino/backup/*)          return 0 ;;
		/etc/machino/models/*)          return 0 ;;
		/usr/bin/machino.old.*)         return 0 ;;
		/usr/bin/machino.machino-new.*) return 0 ;;
		/usr/bin/machino.*-new.*)       return 0 ;;
		/root/.ash_history)             return 0 ;;
		/root/m11a/*)                   return 0 ;;
	esac
	return 1
}

if [ "$REQUEST_METHOD" = "POST" ]; then
	case "$POST_action" in
	del-file)
		_p=$(t_value "POST_path")
		if is_reclaimable "$_p"; then
			rm -f "$_p" && redirect_to "$SCRIPT_NAME" "success" "Deleted $(basename "$_p")" \
			            || redirect_to "$SCRIPT_NAME" "danger" "Could not delete"
		else
			redirect_to "$SCRIPT_NAME" "danger" "Not a cleanup file"
		fi
		;;
	del-dir)
		# Nur die eine bekannte, obsolete Test-Payload -- als Ganzes.
		_p=$(t_value "POST_path")
		case "$_p" in
		/root/m11a)
			[ -d "$_p" ] && rm -rf "$_p" && redirect_to "$SCRIPT_NAME" "success" "Deleted old test payload" \
			             || redirect_to "$SCRIPT_NAME" "danger" "Nothing to delete"
			;;
		*) redirect_to "$SCRIPT_NAME" "danger" "Not a cleanup folder" ;;
		esac
		;;
	ensure-majestic-off)
		if [ -c "$WO" ]; then
			redirect_to "$SCRIPT_NAME" "success" "majestic is already disabled (whiteout present)"
		elif [ -e /rom/etc/init.d/S95majestic ] || [ -e /etc/init.d/S95majestic ]; then
			rm -f "$WO" 2>/dev/null
			if mknod "$WO" c 0 0 2>/dev/null; then
				redirect_to "$SCRIPT_NAME" "success" "majestic disabled (whiteout set); active from next boot"
			else
				redirect_to "$SCRIPT_NAME" "danger" "Could not set the whiteout"
			fi
		else
			redirect_to "$SCRIPT_NAME" "success" "No majestic init script present"
		fi
		;;
	esac
fi

# --- Bereich Overlay (beschreibbar) --------------------------------------
ov_total=$(df -k / 2>/dev/null | awk 'NR==2{print $2}'); [ -n "$ov_total" ] || ov_total=0
ov_free=$(df -k / 2>/dev/null | awk 'NR==2{print $4}');  [ -n "$ov_free" ] || ov_free=0
ov_pct=$(df -k / 2>/dev/null | awk 'NR==2{gsub(/%/,"",$5);print $5}'); [ -n "$ov_pct" ] || ov_pct=0

_glob() { for _g in $1; do [ -f "$_g" ] && printf '%s\n' "$_g"; done; }
RECLAIM=$(
	_glob "/etc/machino/backup/*"
	_glob "/etc/machino/models/*"
	_glob "/usr/bin/machino.old.*"
	_glob "/usr/bin/machino.machino-new.*"
	_glob "/usr/bin/machino.*-new.*"
	_glob "/root/.ash_history"
)
reclaim_kb=0
[ -n "$RECLAIM" ] && reclaim_kb=$(printf '%s\n' "$RECLAIM" | while read -r _f; do [ -f "$_f" ] && du -k "$_f" 2>/dev/null | cut -f1; done | awk '{s+=$1} END{print s+0}')
m11a_kb=0; [ -d /root/m11a ] && m11a_kb=$(du -ks /root/m11a 2>/dev/null | cut -f1)

_cat() {
	case "$1" in
	/etc/machino/backup/*)          echo "majestic/uninstall backup" ;;
	/etc/machino/models/*)          echo "AI model" ;;
	/usr/bin/machino.old.*)         echo "machino rollback backup" ;;
	/usr/bin/machino.machino-new.*|/usr/bin/machino.*-new.*) echo "staged machino update" ;;
	/root/.ash_history)             echo "shell history" ;;
	*)                              echo "reclaimable" ;;
	esac
}

# --- Bereich Firmware/ROM (read-only) ------------------------------------
rom_total=$(df -k /rom 2>/dev/null | awk 'NR==2{print $2}'); [ -n "$rom_total" ] || rom_total=0
rom_pct=$(df -k /rom 2>/dev/null | awk 'NR==2{gsub(/%/,"",$5);print $5}'); [ -n "$rom_pct" ] || rom_pct=100
maj_rom_kb=0; [ -f /rom/usr/bin/majestic ] && maj_rom_kb=$(du -k /rom/usr/bin/majestic 2>/dev/null | cut -f1)
maj_disabled=no; [ -c "$WO" ] && maj_disabled=yes
media_owner=$(streamerctl status 2>/dev/null | sed -n 's/^running:[[:space:]]*//p' | head -1); [ -n "$media_owner" ] || media_owner="unknown"

# --- Bereich Extern (SD/USB) ---------------------------------------------
ext_dev=""
for _d in /dev/mmcblk0 /dev/mmcblk1 /dev/sda /dev/sdb /dev/sdc; do [ -b "$_d" ] && ext_dev="$ext_dev $_d"; done
ext_mount=$(mount 2>/dev/null | grep -iE '/mnt/(mmc|sd|usb)' | awk '{print $3" ("$1")"}' | head -3)

# --- Partitionen (Info) ---------------------------------------------------
%>
<%in p/header.cgi %>

<div id="mch">
<style>
#mch .bar{height:10px;border-radius:5px;background:var(--bs-secondary-bg,#e9ecef);overflow:hidden;margin:6px 0}
#mch .bar>i{display:block;height:100%;background:var(--bs-primary,#0d6efd)}
#mch .bar.ro>i{background:var(--bs-secondary,#6c757d)}
#mch td .btn,#mch td form{margin:0}
#mch th{white-space:nowrap}
#mch .num{font-variant-numeric:tabular-nums}
#mch .drive{font-size:1.4rem;line-height:1;margin-right:.4rem}
</style>

<p class="mj-card-note">Storage on this camera is one flash chip cut into fixed partitions.
  You can only free usable space in the <b>Overlay</b> (the writable area) &mdash; that is where
  machino, its config and any AI model live. The <b>Firmware</b> is read-only; individual files
  there cannot be deleted and would not free overlay space anyway.</p>

<div class="row g-4">

<!-- Bereich: Overlay (beschreibbar) -->
<div class="col-12 col-lg-4"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap"><span class="drive">&#128190;</span>Overlay <small class="text-muted">writable</small></h3><span class="mj-live-rule"></span></div>
	<div class="bar"><i style="width:<%= $ov_pct %>%"></i></div>
	<dl class="row mb-0">
		<dt class="col-7">Free</dt><dd class="col-5 num"><%= $ov_free %> kB</dd>
		<dt class="col-7">Used</dt><dd class="col-5 num"><%= $ov_pct %>%</dd>
		<dt class="col-7">Total</dt><dd class="col-5 num"><%= $ov_total %> kB</dd>
		<dt class="col-7">Reclaimable</dt><dd class="col-5 num">~<%= $reclaim_kb %> kB<% [ "$m11a_kb" -gt 0 ] && echo " + ${m11a_kb}" %></dd>
	</dl>
	<p class="mj-card-note">AI models go in <code>/etc/machino/models</code> here. Free space by
	  deleting the reclaimable files below.</p>
</div></div></div>

<!-- Bereich: Firmware / ROM (read-only) -->
<div class="col-12 col-lg-4"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap"><span class="drive">&#128274;</span>Firmware <small class="text-muted">read-only</small></h3><span class="mj-live-rule"></span></div>
	<div class="bar ro"><i style="width:<%= $rom_pct %>%"></i></div>
	<dl class="row mb-0">
		<dt class="col-7">Size (squashfs)</dt><dd class="col-5 num"><%= $rom_total %> kB</dd>
		<dt class="col-7">majestic (unused, replaced)</dt><dd class="col-5 num"><% if [ "$maj_rom_kb" -gt 0 ]; then echo "${maj_rom_kb} kB"; else echo "absent"; fi %></dd>
		<dt class="col-7">Writable cost</dt><dd class="col-5 num">0 kB</dd>
		<dt class="col-7">majestic disabled</dt><dd class="col-5"><%= $maj_disabled %></dd>
		<dt class="col-7">Media owner</dt><dd class="col-5"><%= $media_owner %></dd>
	</dl>
	<p class="mj-card-note">Read-only. A large part of it is dead weight machino does not use
	  &mdash; majestic (<%= $maj_rom_kb %> kB) never runs, and the system <code>libimp</code>
	  (~1 MB) is unused too (machino links its own). But it is <b>trapped</b>: files here cannot be
	  deleted, and even a custom image without majestic would not help the overlay unless the
	  partition boundary is also moved (a bricking risk &mdash; see Partitions). It costs <b>no</b>
	  writable space and is switched off by an overlay whiteout. Re-assert that (e.g. after an
	  update):</p>
	<form action="<%= $SCRIPT_NAME %>" method="post" class="d-inline"
	      onsubmit="return confirm('Re-assert the majestic whiteout so it stays disabled?')">
		<button class="btn btn-sm btn-outline-secondary" type="submit" name="action" value="ensure-majestic-off">Ensure majestic disabled</button>
	</form>
</div></div></div>

<!-- Bereich: Extern (SD/USB) -->
<div class="col-12 col-lg-4"><div class="card h-100"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap"><span class="drive">&#128189;</span>External <small class="text-muted">SD / USB</small></h3><span class="mj-live-rule"></span></div>
	<% if [ -n "$ext_mount" ]; then %>
	<dl class="row mb-0"><% echo "$ext_mount" | while read -r _m; do %><dt class="col-12 text-break"><%= $_m %></dt><% done %></dl>
	<p class="mj-card-note">External storage is present. Large AI models can live here instead of
	  the overlay &mdash; point the model path at it in the <a href="machino-ai.cgi">AI</a> page.</p>
	<% elif [ -n "$ext_dev" ]; then %>
	<p class="mj-card-note">A card/stick is present but not mounted:<code><%= $ext_dev %></code>.</p>
	<% else %>
	<p class="mj-card-note">None mounted. Switch on <b>USB storage</b> on the <a href="machino-usb.cgi">USB</a>
	  page (it needs the port role Wi-Fi or Cellular) and plug a card reader or stick into the hub
	  next to the module &mdash; or a card into the board's slot, if it is wired. A FAT32 medium is
	  mounted by the system under <code>/mnt</code>; large AI models can live there so they do not
	  fill the overlay.</p>
	<% fi %>
</div></div></div>

</div>

<!-- Reclaimable files in the overlay -->
<div class="row g-4 mt-0"><div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Reclaimable files <small class="text-muted">Overlay</small></h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Backups, staged updates, old payloads and AI models on the writable
	  overlay &mdash; the only files that free real space. <b>Download</b> saves a copy first;
	  <b>Delete</b> then frees the space. Active files (the running machino, the pinned WebUI,
	  network drivers) are never listed.</p>
	<% if [ -n "$RECLAIM" ] || [ "$m11a_kb" -gt 0 ]; then %>
	<table class="table table-sm mb-0 align-middle"><thead>
		<tr><th>File</th><th>What</th><th>Size</th><th style="width:1%"></th><th style="width:1%"></th></tr>
	</thead><tbody>
	<% printf '%s\n' "$RECLAIM" | while read -r f; do [ -f "$f" ] || continue; _s=$(du -k "$f" 2>/dev/null | cut -f1); _w=$(_cat "$f") %>
		<tr>
			<td class="text-break"><code><%= $f %></code></td>
			<td><%= $_w %></td>
			<td class="num"><%= $_s %> kB</td>
			<td><a class="btn btn-sm btn-outline-primary" href="machino-cleanup-dl.cgi?path=<%= $f %>" title="Download a copy first">Download</a></td>
			<td><form action="<%= $SCRIPT_NAME %>" method="post" class="d-inline"
			          onsubmit="return confirm('Delete this file? Download it first if you might need it.')">
				<input type="hidden" name="path" value="<%= $f %>">
				<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="del-file">Delete</button>
			</form></td>
		</tr>
	<% done %>
	<% if [ "$m11a_kb" -gt 0 ]; then %>
		<tr>
			<td class="text-break"><code>/root/m11a</code></td>
			<td>old test payload (folder)</td>
			<td class="num"><%= $m11a_kb %> kB</td>
			<td><span class="text-muted small">&mdash;</span></td>
			<td><form action="<%= $SCRIPT_NAME %>" method="post" class="d-inline"
			          onsubmit="return confirm('Delete the old test payload folder /root/m11a?')">
				<input type="hidden" name="path" value="/root/m11a">
				<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="del-dir">Delete</button>
			</form></td>
		</tr>
	<% fi %>
	</tbody></table>
	<% else %>
	<p class="mj-card-note">Nothing to clean up &mdash; the overlay holds no reclaimable files.</p>
	<% fi %>
</div></div></div></div>

<!-- Partitionen (read-only Info) -->
<div class="row g-4 mt-0"><div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Partitions <small class="text-muted">read-only</small></h3><span class="mj-live-rule"></span></div>
	<table class="table table-sm mb-0"><thead><tr><th>Device</th><th>Name</th><th>Size</th></tr></thead><tbody>
	<% grep -v '^dev:' /proc/mtd 2>/dev/null | while read -r _dev _sz _er _nm; do _dev=${_dev%:}; _nm=$(echo "$_nm" | tr -d '"'); _kb=$(( 0x$_sz / 1024 )) %>
		<tr><td class="num"><%= $_dev %></td><td><%= $_nm %></td><td class="num"><%= $_kb %> kB</td></tr>
	<% done %>
	</tbody></table>
	<p class="mj-card-note">The partition boundaries are fixed in the U-Boot environment. Growing the
	  overlay means shrinking rootfs/kernel and rewriting those boundaries &mdash; that wipes the
	  overlay and <b>bricks the camera</b> if it goes wrong (UART recovery only). It is on the
	  project's no-go list and would gain little here (the firmware already fills its partition).
	  So there is no repartition button; free space in the overlay instead.</p>
</div></div></div></div>

</div>

<%in p/footer.cgi %>
