#!/usr/bin/haserl
<%in p/common.cgi %>
<% page_title="AI" %>
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

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Detectors</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">Live from machino (<code>/api/v1/ai/detectors</code>): which detector
	  can run right now and, if not, every reason. <b>motion</b> is the ISP's IVS motion
	  engine; <b>person</b> runs the model above on the NNA. The camera's AI settings
	  (detector, inference rate, enable) are machino's &mdash; the buttons here set them
	  through its API.</p>
	<div id="ai-detmsg" class="alert py-2" hidden></div>
	<table class="table table-sm mb-2"><thead><tr><th>Detector</th><th>Available</th><th>Why not</th><th></th></tr></thead>
	<tbody id="ai-detlist"><tr><td colspan="4" class="mj-card-note">loading &hellip;</td></tr></tbody></table>
	<dl class="row mb-0">
		<dt class="col-7">Detection state</dt><dd class="col-5" id="ai-state">&hellip;</dd>
		<dt class="col-7">Configured detector / model</dt><dd class="col-5" id="ai-cfg">&hellip;</dd>
		<dt class="col-7">Last error</dt><dd class="col-5" id="ai-err">&hellip;</dd>
	</dl>
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
	<p class="mj-card-note">Model files live in <code>/etc/machino/models</code> &mdash; or on a mounted
	  SD card / USB stick (see below). They are quantized Magik models (TransformKit output for the
	  T40). Upload a model bundle below, or copy one on with the Cam-Tool or scp; the AI settings'
	  <code>model path</code> then points at one<% [ -n "$model_cfg" ] && echo " (currently: <code>$model_cfg</code>)" %>.</p>

	<%
	# AP36: gemountete Medien (rw) unter /mnt -- das Ziel des Uploads, sobald
	# eines da ist. Das Overlay ist eng (der 7,6-MB-yolov5s passt nur nach
	# Aufraeumen), die Karte hat Platz: also Vorgabe Karte, abwaehlbar.
	ext_mounts=$(awk '$2 ~ /^\/mnt\// && $4 ~ /(^|,)rw(,|$)/ {print $2}' /proc/mounts 2>/dev/null | head -8)
	ext_first=$(printf '%s\n' "$ext_mounts" | head -1)
	ext_nmounts=$(printf '%s\n' "$ext_mounts" | grep -c .)
	%>
	<p class="mj-card-note">Upload a <b>.tgz</b> bundle containing the model <code>.bin</code>
	  and its <code>manifest.json</code> (backend <code>venus-nna</code>, NNA generation
	  <code>nna1</code>). Incompatible bundles are refused. On the overlay it needs free space
	  (see <a href="machino-cleanup.cgi">Storage</a>); on a mounted card it does not.</p>
	<div class="mb-3">
		<input type="file" id="mdlfile" accept=".tgz,.gz,.tar" style="max-width:22rem">
		<button class="btn btn-sm btn-outline-primary" type="button" id="mdlup">Upload model bundle</button>
		<span id="mdlmsg" class="mj-card-note" style="display:inline-block;margin-left:.5rem"></span>
	</div>
	<% if [ -n "$ext_first" ]; then %>
	<div class="form-check mb-3">
		<input class="form-check-input" type="checkbox" id="mdlcard" checked data-mount="<%= $ext_first %>">
		<label class="form-check-label" for="mdlcard">Save to the card
		<% if [ "$ext_nmounts" -gt 1 ]; then %>
			<select id="mdlmount" class="form-select form-select-sm d-inline-block ms-1" style="width:auto">
			<% for _m in $ext_mounts; do %><option value="<%= $_m %>"><%= $_m %>/models</option><% done %>
			</select>
		<% else %>
			(<code><%= $ext_first %>/models</code>)
		<% fi %>
		&mdash; the overlay stays free; keep the medium in at every boot the detector starts.
		Unticked: <code>/etc/machino/models</code> on the overlay.</label>
	</div>
	<% fi %>
	<script>
	(function () {
		var b = document.getElementById('mdlup');
		if (!b) return;
		b.addEventListener('click', function () {
			var fi = document.getElementById('mdlfile');
			var msg = document.getElementById('mdlmsg');
			var f = fi.files && fi.files[0];
			if (!f) { msg.textContent = 'Choose a .tgz bundle first.'; return; }
			var url = 'machino-ai-upload.cgi';
			var card = document.getElementById('mdlcard');
			if (card && card.checked) {
				var sel = document.getElementById('mdlmount');
				url += '?dest=' + encodeURIComponent(sel ? sel.value : card.dataset.mount);
			}
			b.disabled = true; msg.textContent = 'Uploading ' + f.name + (card && card.checked ? ' to the card' : '') + ' ...';
			fetch(url, { method: 'POST', body: f })
				.then(function (r) { return r.text().then(function (t) { return { ok: r.ok, t: t }; }); })
				.then(function (res) {
					msg.textContent = res.t;
					if (res.ok) setTimeout(function () { location.reload(); }, 900);
					else b.disabled = false;
				})
				.catch(function (e) { msg.textContent = 'Upload failed: ' + e; b.disabled = false; });
		});
	})();
	</script>
	<% if [ "$model_count" -gt 0 ]; then %>
	<table class="table table-sm mb-0"><thead><tr><th>File</th><th>Size</th><th></th></tr></thead><tbody>
	<% for f in "$MODELDIR"/*; do [ -f "$f" ] || continue; _b=$(basename "$f"); _s=$(du -k "$f" 2>/dev/null | cut -f1) %>
		<tr><td class="text-break"><%= $_b %></td><td><%= $_s %> kB</td>
		<td class="text-nowrap">
		<% case "$_b" in *.bin) %>
			<button class="btn btn-sm btn-outline-primary ai-use" type="button"
			        data-path="<%= $MODELDIR %>/<%= $_b %>"
			        title="Sets ai.model_path to this file through machino's API. A running person detector restarts with it.">Use</button>
		<% ;; esac %>
		<form action="<%= $SCRIPT_NAME %>" method="post" class="d-inline"
		          onsubmit="return confirm('Delete this model?')">
			<input type="hidden" name="name" value="<%= $_b %>">
			<button class="btn btn-sm btn-outline-danger" type="submit" name="action" value="del-model">Delete</button>
		</form></td></tr>
	<% done %>
	</tbody></table>
	<% else %>
	<p class="mj-card-note">No models on the camera yet.</p>
	<% fi %>

	<%
	# AP36: Modelle auf einem gemounteten Medium (USB-Kartenleser am Hub oder
	# Kartenslot). Sie bleiben dort -- der 7,6-MB-yolov5s passt nicht auf das
	# Overlay --, "Use" zeigt ai.model_path darauf. Nur Pfade ohne Leerzeichen.
	ext_models=$(find /mnt -maxdepth 3 -type f -name '*.bin' 2>/dev/null | grep -v ' ' | head -20)
	%>
	<div class="mj-live-head mt-3"><h3 class="mj-cap">Models on external storage</h3><span class="mj-live-rule"></span></div>
	<% if [ -n "$ext_models" ]; then %>
	<p class="mj-card-note">Found on a mounted card or stick (<a href="machino-cleanup.cgi">Storage</a>). They stay on
	  the medium; <b>Use</b> points the model path at the file. Put the <code>manifest.json</code> from the model
	  bundle next to the <code>.bin</code>, and keep the medium in at every boot the detector starts.</p>
	<table class="table table-sm mb-0"><thead><tr><th>File</th><th>Size</th><th></th></tr></thead><tbody>
	<% for f in $ext_models; do _s=$(du -k "$f" 2>/dev/null | cut -f1) %>
		<tr><td class="text-break"><%= $f %></td><td><%= $_s %> kB</td>
		<td class="text-nowrap"><button class="btn btn-sm btn-outline-primary ai-use" type="button"
		        data-path="<%= $f %>"
		        title="Sets ai.model_path to this file through machino's API. A running person detector restarts with it.">Use</button></td></tr>
	<% done %>
	</tbody></table>
	<% else %>
	<p class="mj-card-note">No medium with a <code>.bin</code> is mounted. Switch on <b>USB storage</b> on the
	  <a href="machino-usb.cgi">USB</a> page, put the model bundle's <code>.bin</code> and <code>manifest.json</code>
	  on a FAT32 card, and plug the reader into the hub &mdash; the medium appears under <code>/mnt</code>.</p>
	<% fi %>
</div></div></div>

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Models to start with</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">The NNA runs <b>Magik</b> models only, and machino's helper decodes <b>YOLOv5</b> heads:
	  three outputs at strides 8/16/32, the 80 COCO classes, the standard anchors. So any YOLOv5
	  <b>n / s / m</b> converts and runs unchanged (only <i>person</i> is reported); YOLOv8, YOLO11
	  (anchor-free), SSD or classifiers do not fit the helper as it is. Every model below is
	  uploaded with the form above &mdash; on the card by default when one is mounted.</p>
	<table class="table table-sm mb-2"><thead><tr><th>Model</th><th>Size</th><th>Where</th><th>Notes</th></tr></thead><tbody>
		<tr><td class="text-nowrap">yolov5n demo</td><td>2&nbsp;MB</td>
		<td><a href="https://github.com/aresstack/machino/releases/latest/download/machino-nna-model-demo-t40nn.tgz">machino-nna-model-demo-t40nn.tgz</a></td>
		<td>Fits a full overlay. Ultralytics yolov5n (v7.0) exported to ONNX and quantized to 8 bit by machino's CI.
		  First steps start here. Not yet hardware-verified: SiLU on the NNA.</td></tr>
		<tr><td class="text-nowrap">yolov5s</td><td>7.6&nbsp;MB</td>
		<td><a href="https://github.com/aresstack/machino/releases/latest/download/machino-nna-model-t40nn.tgz">machino-nna-model-t40nn.tgz</a></td>
		<td>The toolkit's reference model, more accurate. Needs the card or a cleaned overlay (see
		  <a href="machino-cleanup.cgi">Storage</a>).</td></tr>
		<tr><td class="text-nowrap">yolov5n / s / m weights</td><td>4&ndash;40&nbsp;MB</td>
		<td><a href="https://github.com/ultralytics/yolov5/releases/tag/v7.0" target="_blank" rel="noopener">Ultralytics YOLOv5 v7.0 release</a></td>
		<td>PyTorch checkpoints (AGPL-3.0). Export and convert with the recipe below; yolov5m is
		  ~21&nbsp;MB as Magik and only lives on a card.</td></tr>
		<tr><td class="text-nowrap">Hugging Face</td><td>&ndash;</td>
		<td><a href="https://huggingface.co/models?search=yolov5%20onnx" target="_blank" rel="noopener">search: yolov5 onnx</a>,
		  <a href="https://huggingface.co/models?library=onnx&amp;search=yolov5" target="_blank" rel="noopener">library: onnx</a>,
		  <a href="https://huggingface.co/Ultralytics" target="_blank" rel="noopener">Ultralytics on Hugging Face</a></td>
		<td>A YOLOv5 ONNX with a 640&times;640 <code>images</code> input converts like the demo: the config
		  cuts the graph at its three head convolutions, so an export that still contains the Detect
		  decode is fine. Other input sizes: set INPUT_SHAPE accordingly. Check the license of what you take.</td></tr>
	</tbody></table>
	<p class="mj-card-note mb-2"><b>Export recipe</b> (x86 Linux, Python 3.10+; exactly what the CI job
	  <a href="https://github.com/aresstack/machino/actions/workflows/build-nna-t40.yml" target="_blank" rel="noopener">build-nna-t40 / model-demo</a> runs):</p>
	<pre class="mj-card-note mb-2" style="white-space:pre-wrap">pip install --index-url https://download.pytorch.org/whl/cpu torch torchvision
pip install onnx pandas pyyaml tqdm matplotlib seaborn requests psutil opencv-python-headless ipython gitpython pillow scipy "setuptools&lt;81"
git clone https://github.com/ultralytics/yolov5 &amp;&amp; cd yolov5 &amp;&amp; git checkout v7.0
sed -i 's/opset_version=opset,/opset_version=opset, dynamo=False,/' export.py   # torch 2.9+: keep the legacy exporter
TORCH_FORCE_NO_WEIGHTS_ONLY_LOAD=1 python3 export.py --weights yolov5n.pt --include onnx --opset 12 --imgsz 640 640</pre>
	<p class="mj-card-note mb-0">Then the conversion in the next card with <code>OUTPUT</code> set to
	  <code>/model.24/m.0/Conv_output_0</code>, <code>/model.24/m.1/Conv_output_0</code>,
	  <code>/model.24/m.2/Conv_output_0</code> (the three 255-channel head convolutions; for another
	  export list them with <code>python3 -c "import onnx; m=onnx.load('x.onnx'); print([o for n in m.graph.node if n.op_type=='Conv' and any('model.24.m' in i for i in n.input) for o in n.output])"</code>).
	  Post-training quantization is 8 bit at most; smaller models come from smaller networks (n instead of s)
	  or from the toolkit's 4-bit training path.</p>
</div></div></div>

<div class="col-12"><div class="card"><div class="card-body">
	<div class="mj-live-head"><h3 class="mj-cap">Building a model with the Ingenic SDK</h3><span class="mj-live-rule"></span></div>
	<p class="mj-card-note">The NNA runs <b>Magik</b> models only: a network exported to ONNX (or
	  TensorFlow/TFLite), quantized and serialised for the T40 by Ingenic's
	  <b>TransformKit</b>. The runtime on the camera is Ingenic's <b>Venus</b> library
	  (magik-toolkit, InferenceKit <code>nna1</code>), statically linked into the
	  helper <code>machino-nna</code>. Model and helper must come from the same toolkit
	  revision &mdash; Venus refuses a model built for another version.</p>
	<ol class="mj-card-note mb-2">
		<li><b>Get the toolkit</b> (public Ingenic mirror, pinned revision used by machino's CI):<br>
		  <code>git clone https://github.com/wispytrace/magik-toolkit &amp;&amp; git -C magik-toolkit checkout e511d370dd7ff84664c9140e0590c354947c7eac</code></li>
		<li><b>Train or pick a network.</b> Reference (development only, AGPL weights):
		  <code>Models/post/yolov5s/yolov5s.onnx</code>. Shippable path: your own weights via
		  <code>Models/training/pytorch/Txx_Xs2/persondet</code> (small person detector, 1&ndash;2M parameters &mdash; it
		  also fits the overlay, unlike the 7.6&nbsp;MB yolov5s).</li>
		<li><b>Convert on an x86 Linux host</b> (TransformKit is an x86 binary):<br>
		  <code>cd Models/post/yolov5s &amp;&amp; ../../../TransformKit/magik-transform-tools --inputpath yolov5s.onnx --outputpath ./yolov5s_t40_magik.mk.h --config cfg/magik_t40.cfg --save_quantize_model true</code><br>
		  The config (JSON) sets SOC=T40, INPUT name and INPUT_SHAPE 1&times;3&times;640&times;640, COLOR RGB, MEAN 0, NORMAL 255,
		  OUTPUT (the tensors to cut the graph at &mdash; the three head convolutions) and QUANT_DATASET_PATH
		  (a folder of 10&ndash;20 typical images; the toolkit ships <code>yolov5-20</code>). Output: <code>yolov5s_t40_magik.bin</code>.
		  The tool prints &bdquo;Convert successfully&ldquo; and still exits 1 &mdash; trust the text and the file.
		  Documentation: <code>Docs/en/Magik Post-Training-Quantization User Guide.pdf</code> in the toolkit.</li>
		<li><b>Write the manifest</b> next to the .bin (schemaVersion 1). Minimum:<br>
		  <code>{"schemaVersion":1,"id":"my-model","backend":"venus-nna","nnaGeneration":"nna1","soc":"t40nn","modelFile":"yolov5s_t40_magik.bin","classes":[{"id":0,"label":"person"}]}</code><br>
		  machino refuses a model whose manifest names another backend, NNA generation, SoC or file (reason codes <code>AI_MODEL_INCOMPATIBLE_*</code>).</li>
		<li><b>Bundle and upload:</b> <code>tar czf my-model.tgz yolov5s_t40_magik.bin manifest.json</code>, then the upload form above, then <b>Use</b> next to the file and select the <b>person</b> detector.</li>
	</ol>
	<p class="mj-card-note mb-0">Ready-made: the CI workflow
	  <a href="https://github.com/aresstack/machino/actions/workflows/build-nna-t40.yml" target="_blank" rel="noopener">build-nna-t40</a>
	  produces the helper (<code>machino-nna-t40</code>) and two upload-ready bundles
	  (<code>machino-nna-model-bundle</code> = yolov5s, <code>machino-nna-model-demo-bundle</code> = yolov5n) from
	  these exact recipes; every release carries both as <code>machino-nna-model-t40nn.tgz</code> and
	  <code>machino-nna-model-demo-t40nn.tgz</code>.
	  Details, evidence and the boot prerequisites (nmem window, soc-nna driver):
	  <a href="https://github.com/aresstack/machino/blob/main/machino/docs/architecture/nna-model.md" target="_blank" rel="noopener">nna-model.md</a>,
	  <a href="https://github.com/aresstack/machino/blob/main/machino/docs/architecture/nna.md" target="_blank" rel="noopener">nna.md</a>,
	  <a href="https://github.com/aresstack/machino/wiki/AI-Person-Detection" target="_blank" rel="noopener">wiki: AI &amp; Person Detection</a>.
	  The public magik-toolkit carries no LICENSE file; machino ships nothing from the stock firmware.</p>
</div></div></div>

</div>
</div>

<script>
// Lokal (IIFE): header.cgi laedt /a/main.js, das global `function $` deklariert.
(function () {
"use strict";
const $ = (id) => document.getElementById(id);

async function api(method, path, body) {
  const r = await fetch(path, {
    method,
    headers: body === undefined ? {} : {"Content-Type": "application/json"},
    body: body === undefined ? undefined : JSON.stringify(body),
    credentials: "same-origin"
  });
  if (r.status === 401) { location.href = "/login.html?next=/cgi-bin/machino-ai.cgi"; throw new Error("unauthorized"); }
  let j = null;
  try { j = await r.json(); } catch (e) { j = null; }
  return {status: r.status, body: j};
}
function msg(el, text, kind) {
  el.textContent = text;
  el.className = "alert py-2 " + (kind === "ok" ? "alert-success" : kind === "bad" ? "alert-danger" : "alert-secondary");
  el.hidden = !text;
}
function reason(res, fallback) {
  const e = res.body && (res.body.error || res.body);
  return (e && (e.message || e.error)) || fallback || ("HTTP " + res.status);
}
function td(tr, text, cls) { const c = document.createElement("td"); if (cls) c.className = cls; c.textContent = text; tr.appendChild(c); return c; }

async function patchAi(fields, okText) {
  const m = $("ai-detmsg");
  msg(m, "applying \u2026", "");
  const r = await api("PATCH", "/api/v1/config", {ai: fields});
  if (r.status === 200) { msg(m, okText, "ok"); await refresh(); }
  else msg(m, reason(r), "bad");
}

async function refresh() {
  const tb = $("ai-detlist");
  // The three reads are independent -- fire them together, one round-trip
  // instead of three on the camera's slow HTTP path.
  const [d, t, c] = await Promise.all([
    api("GET", "/api/v1/ai/detectors"),
    api("GET", "/api/v1/telemetry"),
    api("GET", "/api/v1/config"),
  ]);
  tb.innerHTML = "";
  if (d.status !== 200 || !d.body || !d.body.detectors) {
    const tr = document.createElement("tr"); td(tr, reason(d, "machino API not reachable"), "mj-card-note").colSpan = 4; tb.appendChild(tr);
  } else {
    for (const det of d.body.detectors) {
      const tr = document.createElement("tr");
      td(tr, det.label + (det.id === d.body.selected ? "  (selected)" : ""));
      td(tr, det.available ? "yes" : "no");
      const why = td(tr, "", "mj-card-note");
      if (det.reasons && det.reasons.length) {
        for (const rs of det.reasons) {
          const line = document.createElement("div");
          const code = document.createElement("code"); code.textContent = rs.code;
          line.appendChild(code); line.appendChild(document.createTextNode(" " + (rs.message || "")));
          why.appendChild(line);
        }
      } else why.textContent = "\u2014";
      const act = td(tr, "", "text-nowrap");
      const b = document.createElement("button");
      b.type = "button"; b.className = "btn btn-sm btn-outline-primary";
      b.textContent = det.id === d.body.selected ? "Selected" : "Select + enable";
      b.disabled = det.id === d.body.selected;
      b.title = det.available ? "Sets ai.detector and ai.enabled through machino's API"
                              : "Selectable anyway (machino keeps the choice); it starts in state=error until every reason is green";
      b.addEventListener("click", () => patchAi({detector: det.id, enabled: true}, "detector " + det.id + " selected and enabled"));
      act.appendChild(b);
      tb.appendChild(tr);
    }
  }
  const ai = t.body && t.body.ai;
  $("ai-state").textContent = ai ? (ai.state + (ai.backend ? " (" + ai.backend + ")" : "")) : "\u2014";
  const cfg = c.body && c.body.ai;
  $("ai-cfg").textContent = cfg ? (cfg.detector + " / " + (cfg.model_path || "(no model path)")) : "\u2014";
  // The Error-state reason rides in telemetry's ai.error {code,message}
  // (see telemetry_json) -- there is no ai.last_error field there.
  const e = ai && ai.error ? ((ai.error.code || "") + " " + (ai.error.message || "")).trim() : "";
  $("ai-err").textContent = e || "\u2014";
}

document.querySelectorAll(".ai-use").forEach((b) => {
  b.addEventListener("click", () => patchAi({model_path: b.dataset.path}, "model path set to " + b.dataset.path));
});
refresh().catch((e) => msg($("ai-detmsg"), "machino API: " + e, "bad"));
})();
</script>

<%in p/footer.cgi %>
