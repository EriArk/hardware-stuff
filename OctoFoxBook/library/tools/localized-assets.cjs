// Use the production asset renderer in browser fixtures, without a live account.
const {execFileSync} = require('node:child_process');
const fs = require('node:fs'), path = require('node:path');
const english = process.env.QA_LANGUAGE === 'en';
const localized = english ? JSON.parse(execFileSync(process.env.PYTHON || 'python', ['-c',
  "import base64,json; from octofox_library.localization import WEB,asset; print(json.dumps({p.name:base64.b64encode(asset(p.name,'en')).decode() for p in WEB.iterdir() if p.suffix in {'.html','.js','.webmanifest'}}))"],
  {encoding:'utf8',windowsHide:true,env:{...process.env,PYTHONPATH:path.resolve('src'),PYTHONUTF8:'1'}})) : {};
module.exports = {english, readAsset(file) {
  const encoded = localized[path.basename(file)];
  return encoded ? Buffer.from(encoded, 'base64') : fs.readFileSync(file);
}};
