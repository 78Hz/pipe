// Renders docs/manual/manual.html to PDF.
//   node docs/manual/render.js <version> <output.pdf>
const { chromium } = require('playwright');
const fs = require('fs');
const path = require('path');
(async () => {
  const version = process.argv[2] || 'dev';
  const out = path.resolve(process.argv[3] || 'Pipe-User-Manual.pdf');
  const src = path.join(__dirname, 'manual.html');
  const tmp = path.join(__dirname, '.manual.render.html');
  fs.writeFileSync(tmp, fs.readFileSync(src, 'utf8').replace(/\{\{VERSION\}\}/g, version));
  const browser = await chromium.launch();
  const page = await browser.newPage();
  await page.goto('file://' + tmp, { waitUntil: 'load' });
  await page.evaluate(() => document.fonts.ready);
  await page.pdf({ path: out, format: 'A4', printBackground: true, preferCSSPageSize: true });
  await browser.close();
  fs.unlinkSync(tmp);
  console.log('wrote', out);
})();
