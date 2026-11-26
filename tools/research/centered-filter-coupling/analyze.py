#!/usr/bin/env python3
"""Read actual combined timestamps; never sum the two older studies."""
import json
import os
from pathlib import Path
import sys

RUN = Path(os.environ["CRYSTALHD_COUPLING_RUN_DIR"]).resolve()
PERIOD_NUMERATOR = 1001000000
PERIOD_DENOMINATOR = 30

def need(condition, message):
    if not condition:
        raise ValueError(message)

def metric(values):
    values = sorted(values)
    need(values and all(isinstance(v, int) and v >= 0 for v in values), 'nonnegative actual durations')
    return dict(count=len(values), mean_ns=sum(values)/len(values),
                p50_ns=values[(len(values)-1)//2],
                p95_ns=values[(len(values)*95+99)//100-1], max_ns=values[-1])

def analyze(case):
    result = json.loads((case/'result.json').read_text())
    need(result['status'] == 'PASS_SCOPED_COMBINED_CURRENT_OWN_AND_ORIGINAL_FILTER', 'actual passing execution')
    records = [json.loads(line) for line in (case/'raw.jsonl').read_text().splitlines()]
    headers = [r for r in records if r.get('type') == 'coupled_header']
    rows = [r for r in records if r.get('type') == 'coupled_cycle']
    need(len(headers) == 1, 'one actual combined header')
    head = headers[0]
    validation = head.get('validation_mode', 'FULL_BYTES_EACH_MEASURED_INPUT_AND_OUTPUT')
    full_pixels = validation == 'FULL_BYTES_EACH_MEASURED_INPUT_AND_OUTPUT'
    need(full_pixels or validation == 'TIMING_AFTER_SEPARATE_FULL_QUALIFICATION', 'explicit supported validation scope')
    need(len(rows) == head['inputs'] and head['outputs'] == 2*(len(rows)-1), 'complete input/output counts')
    period = lambda i: i*PERIOD_NUMERATOR//PERIOD_DENOMINATOR
    pts = lambda i: 66734+(i*1001000+15)//30
    first_token = rows[0]['token']
    readiness = []
    stage_values = {}
    selected = []
    gaps = []
    for i, row in enumerate(rows):
        need(row['index'] == i and row['token'] == (i+1)*first_token, 'linear original input/owned token mapping')
        need(row['due_ns'] == head['epoch_ns']+period(i) and
             row['due_ns'] <= row['begin_ns'] <= row['end_ns'], 'indexed nominal release and actual row')
        need(row['write_epoch_after'] > row['write_epoch_before'], 'actual backing write epoch advances')
        need(row['live_images_at_write'] == int(head['hold_image_control'] and i > 0), 'actual image/backing overlap census')
        fields = row['fields']
        need(len(fields) == (2 if i else 0), 'one-input lookahead with no implicit EOF')
        spans = []
        for group in ('owned_stages', 'stages'):
            for name, s in row[group].items():
                if not s['executed']:
                    continue
                need(row['begin_ns'] <= s['wall_begin_ns'] <= s['wall_end_ns'] <= row['end_ns'] and
                     s['thread_cpu_begin_ns'] <= s['thread_cpu_end_ns'], 'valid nested measurement endpoints')
                wall = s['wall_end_ns']-s['wall_begin_ns']
                cpu = s['thread_cpu_end_ns']-s['thread_cpu_begin_ns']
                stage_values.setdefault(group+'/'+name, dict(wall=[], thread_cpu=[]))
                stage_values[group+'/'+name]['wall'].append(wall)
                stage_values[group+'/'+name]['thread_cpu'].append(cpu)
                spans.append(s)
        spans.sort(key=lambda s:s['wall_begin_ns'])
        need(all(a['wall_end_ns'] <= b['wall_begin_ns'] for a,b in zip(spans, spans[1:])),
             'selected same-thread stage intervals are disjoint')
        union = sum(s['wall_end_ns']-s['wall_begin_ns'] for s in spans)
        selected.append(union)
        gaps.append(row['end_ns']-row['begin_ns']-union)
        for f, field in enumerate(fields):
            need(field['pts'] == (2*pts(i-1) if f == 0 else pts(i-1)+pts(i)) and
                 field['best'] == pts(i-1), 'actual field metadata')
            need((full_pixels and field['all_pixels_equal'] is True) or
                 (not full_pixels and field['all_pixels_equal'] is None), 'truthful measured pixel-validation scope')
            need(field['ready_ns'] == row['stages']['filter_get'+str(f)+'_wrapper']['wall_end_ns'], 'actual successful GET-wrapper return')
            # Nominal source-field presentation coordinates, explicitly assumed.
            nominal = (2*(i-1)+f)*PERIOD_NUMERATOR//(2*PERIOD_DENOMINATOR)
            readiness.append(field['ready_ns']-head['epoch_ns']-nominal)
    need(len(readiness) == head['outputs'], 'all fields considered')
    cycle = [r['end_ns']-r['begin_ns'] for r in rows]
    required = max(readiness)
    lead = 100100000
    advance = 40000000
    summary = dict(
        status='PASS_SCOPED_COMBINED_CURRENT_OWN_AND_ORIGINAL_FILTER',
        validation_mode=validation,
        measured_outputs_individually_full_pixel_checked=head['outputs'] if full_pixels else 0,
        actual_inputs=head['inputs'], actual_outputs=head['outputs'],
        actual_same_canvas_overlap_writes=head['same_canvas_overlap_writes'],
        forced_image_hold_control=head['hold_image_control'],
        cycle_wall=metric(cycle),
        steady_cycle_wall=metric(cycle[1:]),
        cycles_exceeding_nominal=sum(v*PERIOD_DENOMINATOR > PERIOD_NUMERATOR for v in cycle[1:]),
        actual_input_release_lateness=metric([r['begin_ns']-r['due_ns'] for r in rows]),
        actual_get_readiness_required_lead_ns_at_zero_advance=required,
        conditional_required_total_lead_ns_at40ms_advance=required+advance,
        conditional_misses_at100_1ms_lead40ms_advance=sum(v+advance > lead for v in readiness),
        conditional_margin_ns=lead-advance-required,
        selected_disjoint_stage_wall=metric(selected),
        uninstrumented_cycle_gap_wall=metric(gaps),
        stage_metrics={name:{kind:metric(values) for kind,values in kinds.items()}
                       for name,kinds in stage_values.items()},
        measured_process_cpu_ns=head['process_cpu_end_ns']-head['process_cpu_begin_ns'],
        measured_trial_wall_ns=head['trial_end_ns']-head['trial_begin_ns'],
        source_revision=head['source_revision'], final_cow_live_bytes=head['final_cow_live_bytes'],
        source_eof_injected=False, stock_or_release_qualification=False,
        limits=[
            'Three synthetic packed-source patterns, repeated over unique900 timestamps; not900 distinct decoded originals.',
            'Real current callbacks and original BWDIF execute together. No sum of separate historical timings.',
            'OWN ordinary memcpy of visible1080 rows from coded1088 canvas; not original Kodi SSE-copy or its actual runtime schedule.',
            'No DIL receive/release, decoder, DMA, GPU, natural field gap, renderer/media clock or other application workload.',
            ('Admission includes fixture validation. Producer full-byte checks and output-oracle comparisons lie in cycle gaps; input-oracle comparison has its own span. They change cache state and GET1 timing.'
             if full_pixels else
             'Timing mode excludes explicit full-canvas/client-input/filter-output byte oracles inside pacing. It retains fixture validation and deferred-Small untouched-storage copy/equality guards, so it is not pure product cost. Its900 measured inputs/1798outputs are NOT individually full-byte qualified; separate retained full900 correctness and mandatory pre-pacing qualification retain that scope.'),
            'GET-wrapper intervals include bridge metadata validation. Readiness is actual wrapper return for this instrumented workload, not raw unwrapped API exit.',
            'Process CPU includes actual filter helper threads and all within-trial checks; thread-CPU stage sums are not whole-process CPU.',
            'Graph reference derives full outputs from independent scalar input bytes with same unchanged provider; not an independent deinterlacing implementation or normative decoder golden.',
            'Positive held8 control forces actual overlap; ordinary900 result qualifies only OWN copy-and-release path, not original Kodi same-backing lifetime.',
            'Virtual100.1ms lead/40ms advance is an inherited explicit comparison assumption, not measured Kodi buffer, scheduling guarantee or A/V proof.',
            'No performance improvement or candidate promotion follows from this one trial. Original Source900 failures unchanged.',
        ])
    (case/'analysis.json').write_text(json.dumps(summary, indent=2)+'\n')
    return {k:summary[k] for k in ('status','validation_mode','measured_outputs_individually_full_pixel_checked','actual_inputs','actual_outputs','actual_same_canvas_overlap_writes',
        'steady_cycle_wall','cycles_exceeding_nominal','conditional_required_total_lead_ns_at40ms_advance',
        'conditional_misses_at100_1ms_lead40ms_advance','conditional_margin_ns','final_cow_live_bytes')}

if __name__ == '__main__':
    need(len(sys.argv) == 2, 'one actual case name required')
    print(json.dumps(analyze(RUN/sys.argv[1]), indent=2))
