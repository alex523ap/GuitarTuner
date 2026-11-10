// PebbleKit JS entry for Guitar Tuner
var BASE64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';

function toBase64(bytes) {
  var out = '';
  for (var i = 0; i < bytes.length; i += 3) {
    var a = bytes[i];
    var b = bytes[i + 1];
    var c = bytes[i + 2];
    out += BASE64[a >> 2];
    out += BASE64[((a & 3) << 4) | ((b || 0) >> 4)];
    out += (i + 1 < bytes.length) ? BASE64[((b & 15) << 2) | ((c || 0) >> 6)] : '=';
    out += (i + 2 < bytes.length) ? BASE64[c & 63] : '=';
  }
  return out;
}

Pebble.addEventListener('ready', function() {
  console.log('Guitar Tuner PKJS ready');
});

// Debug builds only send these: clips logged here become WAVs with scripts/clip_logs_to_wav.py
Pebble.addEventListener('appmessage', function(e) {
  var p = e.payload;
  if (p.CLIP_LABEL !== undefined) {
    console.log('CLIP|begin|' + p.CLIP_ID + '|' + p.CLIP_LABEL + '|' + p.CLIP_SAMPLES + '|' +
                p.CLIP_TOTAL + '|' + p.CLIP_PEAK + '|' + p.CLIP_LEVEL);
  } else if (p.CLIP_DATA !== undefined) {
    console.log('CLIP|data|' + p.CLIP_ID + '|' + p.CLIP_SEQ + '|' + toBase64(p.CLIP_DATA));
  } else if (p.CLIP_SUM !== undefined) {
    console.log('CLIP|end|' + p.CLIP_ID + '|' + p.CLIP_SUM);
  }
});
