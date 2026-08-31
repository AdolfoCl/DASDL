#!/usr/bin/env python3
"""Assemble the browser build: one self-contained HTML file.

The page, the samples and the WebAssembly module (already base64-inlined by
emcc -sSINGLE_FILE) go into a single file, so it runs from a static host, from
a file:// URL, or anywhere that will not let a page fetch a second resource.

Two outputs from the same body: dasdl.html is a whole document, and
dasdl.embed.html is the same content without the surrounding document tags,
for a host that supplies its own.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def main(outdir):
    with open(os.path.join(HERE, 'dasdl.html.in'), encoding='utf-8') as f:
        page = f.read()

    with open(os.path.join(outdir, 'dasdl.js'), encoding='utf-8') as f:
        module = f.read()

    sampledir = os.path.join(ROOT, 'samples')
    samples = {}
    for name in sorted(os.listdir(sampledir)):
        if name.endswith('.dasdl'):
            with open(os.path.join(sampledir, name), encoding='utf-8') as f:
                samples[name[:-len('.dasdl')]] = f.read()
    if not samples:
        sys.exit('bundle: no samples found in ' + sampledir)

    # </script> inside a string literal would close the tag that holds it.
    blob = json.dumps(samples, ensure_ascii=False).replace('</', '<\\/')

    with open(os.path.join(HERE, 'fonts.css'), encoding='utf-8') as f:
        page = page.replace('/*DASDL-FONTS-HERE*/', f.read())

    page = page.replace('//DASDL-SAMPLES-HERE', 'var SAMPLES = ' + blob + ';')
    page = page.replace('//DASDL-JS-HERE', module)

    whole = ('<!doctype html>\n<meta charset="utf-8">\n'
             '<meta name="viewport" content="width=device-width,initial-scale=1">\n'
             + page)
    write(os.path.join(outdir, 'dasdl.html'), whole)
    write(os.path.join(outdir, 'dasdl.embed.html'), page)
    print('bundle: %d samples, %.1f MB' % (len(samples), len(whole) / 1e6))


def write(path, text):
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'web'))
