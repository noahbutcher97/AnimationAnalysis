"""Publish bounded surface measurements and portable RGB/label evidence."""
import argparse
import base64
import hashlib
from html import escape
import io
import json
from pathlib import Path

from ..artifacts import atomic_text

from ..adapters.unreal_surface import load_surface_bundle
from ..surfaces import measure_surface_relation


def publish(capture, first, second, tolerance, output):
    capture, output = Path(capture).resolve(), Path(output).resolve()
    if output.suffix != '.html' or output.is_relative_to(capture) or output.with_suffix('.json').resolve().is_relative_to(capture):
        raise ValueError('Use an HTML output outside the immutable input bundle')
    output.parent.mkdir(parents=True, exist_ok=True)
    atomic_text(output, '<!doctype html><p>Surface analysis incomplete.</p>')
    atomic_text(output.with_suffix('.json'), json.dumps({'status':'inconclusive','reason':'Analysis incomplete'}))
    manifest, frames, hashes = load_surface_bundle(capture)
    if first == second or first not in manifest['subjects'] or second not in manifest['subjects']:
        raise ValueError('Choose two distinct declared subjects')
    first_id,second_id=manifest['subjects'][first],manifest['subjects'][second]
    from PIL import Image
    def png(image):
        buffer=io.BytesIO(); image.save(buffer,format='PNG'); data=buffer.getvalue()
        return 'data:image/png;base64,'+base64.b64encode(data).decode(),hashlib.sha256(data).hexdigest()
    evidence, measurements, articles=[],[],[]
    for index,frame in enumerate(frames):
        meta=frame['metadata']; width,height=meta['width'],meta['height']
        result=measure_surface_relation(frame['labels'],frame['scene_depth_cm'],frame['label_depth_cm'],width,height,first_id,second_id,tolerance)
        measurements.append(dict(file=meta['file'],**result))
        image=Image.frombytes('RGBA',(width,height),frame['bgra'],'raw','BGRA').convert('RGB')
        raw,image_hash=png(image)
        overlay=Image.new('RGBA',(width,height)); pixels=bytearray(width*height*4)
        for i,label in enumerate(frame['labels']):
            if label in (first_id,second_id):
                pixels[4*i:4*i+4]=bytes((255,170,0,130) if label==first_id else (0,220,255,130))
        overlay.frombytes(bytes(pixels))
        shown,_=png(Image.alpha_composite(image.convert('RGBA'),overlay).convert('RGB'))
        evidence.append(dict(file=meta['file'],width=width,height=height,simulation_time_s=meta['simulation_time_s'],
                             time=dict(domain='simulation',seconds=meta['simulation_time_s']),
                             image=raw,image_sha256=image_hash,engine_frame=meta['engine_frame'],
                             world_to_clip_row_major=meta['world_to_clip_row_major'],surface_measurements=result))
        gap=result['minimum_visible_pixel_center_distance_px']
        detail=f'{gap:.2f} px between nearest visible pixel centres' if gap is not None else 'Distance indeterminate'
        articles.append(f'<article id="frame-{index}"><h2>{escape(meta["file"])}</h2><p>{escape(detail)}. '
                        f'Renderer frame {meta["engine_frame"]}; simulation {meta["simulation_time_s"]:.3f}s.</p>'
                        f'<img src="{raw}" alt="Original RGB"><img src="{shown}" alt="Custom-depth labels over RGB">'
                        f'<pre>{escape(json.dumps(result,indent=2))}</pre></article>')
    payload=dict(schema_version=2,capture=str(capture),input_hashes=hashes,frames=evidence)
    data=json.dumps(payload).replace('<','\\u003c')
    html='<!doctype html><meta charset="utf-8"><title>Surface observation review</title><style>body{font:16px system-ui;background:#151b24;color:#ecf0f4;max-width:1400px;margin:28px auto;padding:0 20px}article{border-top:1px solid #526273;margin-top:24px}img{width:49%;height:auto}pre{white-space:pre-wrap}</style>'
    html+=f'<h1>Surface observation review</h1><p>Raw RGB / custom-depth labels: orange {escape(first)}, cyan {escape(second)}. '
    html+='Labels can show through ordinary scene occluders; they are not full silhouettes behind other labelled objects. Measurements use scene depth to classify the observed label pixels.</p>'
    html+='<p>Measured separation or adjacency is not a 3D contact, penetration or animation-quality verdict.</p>'+''.join(articles)
    html+=f'<script id="visual-evidence" type="application/json">{data}</script>'
    html+='<script>const p=new URLSearchParams(location.hash.slice(1));if(p.has("frame")){const n=Number(p.get("frame"));if(Number.isInteger(n))document.getElementById("frame-"+n)?.scrollIntoView();}</script>'
    atomic_text(output, html)
    library=Path(__file__).resolve().parents[1]
    implementation={name:hashlib.sha256((library/name).read_bytes()).hexdigest() for name in ('surfaces.py','adapters/unreal_surface.py','jobs/surface_review.py','artifacts.py','integrity.py','errors.py')}
    report=dict(status='measurement_recorded',subjects=[first,second],input_hashes=hashes,implementation_hashes=implementation,
                evidence_sha256=hashlib.sha256(output.read_bytes()).hexdigest(),measurements=measurements,
                limits='Uncalibrated artistic criteria; per-observation visible raster measurements only. No temporal persistence or mesh intersection assertion.')
    atomic_text(output.with_suffix('.json'), json.dumps(report,indent=2))
    return report


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--capture',type=Path,required=True)
    parser.add_argument('--first',required=True)
    parser.add_argument('--second',required=True)
    parser.add_argument('--visibility-tolerance-cm',type=float,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    try:
        result=publish(args.capture,args.first,args.second,args.visibility_tolerance_cm,args.output)
    except (OSError,ValueError,KeyError,TypeError,ImportError) as error:
        parser.exit(1,f'Surface review unavailable: {error}\n')
    print(f'{result["status"]}: {args.output}; {len(result["measurements"])} observations')


if __name__=='__main__': main()
