const glslangMod = require('@webgpu/glslang');
const fs = require('node:fs');
const path = require('node:path');
glslangMod().then(m => {
  for (const f of fs.readdirSync('glsl')) {
    const match = f.match(/^(.+)\.(vert|frag)$/);
    if (!match) continue;
    const src = fs.readFileSync(path.join('glsl', f), 'utf8');
    const stage = match[2] === 'vert' ? 'vertex' : 'fragment';
    const out = m.compileGLSL(src, stage, false);
    fs.writeFileSync(path.join('..', '..', 'src', 'native_runtime', `${match[1]}.${match[2]}.spv`), Buffer.from(out.buffer, out.byteOffset, out.byteLength));
    console.log('ok', f, out.length, 'words');
  }
  process.exit(0);
}).catch(e => { console.error('ERR', e); process.exit(1); });
