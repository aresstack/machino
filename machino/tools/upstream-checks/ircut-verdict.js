// AP18: run the UNMODIFIED upstream IR-cut checker against the nightMode
// Machino actually serves. The question is not what we intended to publish -
// it is what the stock Night page concludes when it reads us, and the only
// way to know that is to let its own 1753 lines decide.
//
//   node tools/upstream-checks/ircut-verdict.js <majestic-webui-clone> <cfg.json>
//
// where cfg.json is what GET /api/v1/config.json returned from the camera.
// Expected: ZERO findings. Anything else means the stock page has something to
// say about this camera, and it is worth knowing what before an owner does.
//
// The two controls at the end are the point: they show that the single line we
// do publish is load-bearing in both directions - drop it and the page raises
// a red hardware fault, invent a pin and the page starts describing a filter
// this board has no evidence of having.
const path = require('path');
const ic = require(path.join(process.argv[2] || '../majestic-webui', 'www', 'a', 'ircut-check.js'));
const pads = require(path.join(process.argv[2] || '../majestic-webui', 'www', 'a', 'ircut-pads.js'));
const cfg = JSON.parse(require('fs').readFileSync(process.argv[3] || 'cfg.json', 'utf8'));
const nm = cfg.nightMode || {};

console.log('nightMode as served :', JSON.stringify(nm));
console.log('wiki pin table, t40 :', JSON.stringify(pads.forSoc('t40nn')));
console.log('wiki pin table, t31 :', JSON.stringify(pads.forSoc('t31')).slice(0, 60) + ' ...  (a part it does know)');

// No heartbeat sample and no observation: that is a camera that publishes no
// night telemetry, which is exactly this one.
for (const [label, sample, obs] of [
    ['no sample, no observation', null, null],
    ['a heartbeat with nothing to say', { night: 0, ircut: 0, light: 0, src: null }, null],
]) {
    const out = ic.diagnose(nm, sample, obs) || [];
    console.log('\n-- ' + label + ': ' + out.length + ' finding(s)');
    for (const f of out) console.log('   [' + f.level + '] ' + f.id + ' - ' + f.title);
}

// And the control: what the SAME checker says if we had invented a pin.
const lying = Object.assign({}, nm, { irCut: 'auto', irCutPin1: 11 });
const out2 = ic.diagnose(lying, null, null) || [];
console.log('\n-- control, if a pin were invented (irCut auto, irCutPin1 11): ' + out2.length + ' finding(s)');
for (const f of out2) console.log('   [' + f.level + '] ' + f.id + ' - ' + f.title);

// And what an ABSENT nightMode would say - the state before the compat layer
// started answering at all.
const out3 = ic.diagnose({}, null, null) || [];
console.log('\n-- control, nightMode absent entirely: ' + out3.length + ' finding(s)');
for (const f of out3) console.log('   [' + f.level + '] ' + f.id + ' - ' + f.title);
