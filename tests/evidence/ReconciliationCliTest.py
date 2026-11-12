#!/usr/bin/env python3
"""Public CLI checks with a fixed ledger and a fake GitHub boundary."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
CLI = ROOT / 'scripts/evidence/reconcile_1058.py'


class ReconciliationCliTest(unittest.TestCase):
    # Frozen candidate snapshot (#1249): the exact bytes `git show
    # a4df1a115:docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json`
    # produced. The CI checkout uses fetch-depth: 1, so that object is absent
    # there and the comparison below cannot read it from repository history.
    HISTORICAL_LEDGER = (ROOT / 'fixtures/pi-v1.3.0/reconciliation-1183'
                         / '1058-consolidated-capability-ledger-at-a4df1a115.json')
    HISTORICAL_LEDGER_BLOB = 'b55037071e21c0ee470dee05a23af1fd31fd0402'

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        files = {
            'docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json': {'rows': [
                {'id': 'r1', 'owner': 'owner', 'membership': 'supported', 'implementation': 'missing', 'verification': 'unverified', 'implementation_ticket': None},
                {'id': 'r2', 'owner': 'owner', 'membership': 'supported', 'implementation': 'partial', 'verification': 'upstream-captured', 'implementation_ticket': '#2'},
                {'id': 'r3', 'owner': 'owner', 'membership': 'supported', 'implementation': 'missing', 'verification': 'unverified', 'implementation_ticket': '#1058-draft'},
                {'id': 'r4', 'owner': 'owner', 'membership': 'supported', 'implementation': 'implemented', 'verification': 'unverified', 'implementation_ticket': None},
            ]},
            'docs/research/pi-v1.3.0/1058-deliveries.json': {'records': [{'id': 'receipt', 'ticket': '2', 'rows': [{'id': 'r2'}]}]},
            'docs/research/pi-v1.3.0/1058-implementation-frontier.json': {'ownerGraph': {'nodes': [{'owner': 'owner'}]}, 'residualReconciliation': [{'issue': '#5'}]},
            'fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json': {'ticketMappings': {'entries': [{'id': 'mapping', 'ticket': '#2', 'history': True, 'rows': ['r2']}, {'id': 'mapping4', 'ticket': '#4', 'rows': ['r1']}]}},
            'docs/research/pi-v1.3.0/1182-tui-route-batches.json': {'batches': [{'id': 'B1', 'owner': 'owner', 'rows': ['r1'], 'dependency': 'capture'}]},
        }
        for path, value in files.items():
            target = self.root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(json.dumps(value))
        wiki = self.root / 'docs/wiki/pi-alignment'
        wiki.mkdir(parents=True)
        proposal = (ROOT / 'docs/wiki/pi-alignment/reconciliation-1183-proposal-2026-11-04.md').read_text()
        (wiki / 'reconciliation-1183-proposal-2026-11-04.md').write_text(proposal.split('## 3.')[0] + '## 3. Next\n[broken](missing.md) #99\n')
        mapping = self.root / 'scripts/evidence/1058-successors.json'
        mapping.parent.mkdir(parents=True)
        mapping.write_text(json.dumps({'version': 1, 'scope': 'fixture', 'entries': []}))
        self.mapping = mapping
        self.historical = self.root / 'historical.json'
        self.historical.write_text(json.dumps(files['docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json']))
        (wiki / 'decisions-2026-11-04.md').write_text('| N5 → B1 | [#3](https://github.com/example/repo/issues/3) | TUI batch |\n')
        self.fake = self.root / 'gh'
        self.fake.write_text(f'#!{sys.executable}\n' + '''import json, os, re, sys
args = dict(x.split('=', 1) for x in sys.argv if '=' in x)
if args.get('lint') == '1':
    if os.environ.get('GH_LINT_FAIL'):
        print(json.dumps({'errors': [{'message': 'lint denied'}], 'data': {}})); sys.exit(0)
    numbers = re.findall(r'i([0-9]+): issue', args['query'])
    print(json.dumps({'data': {'repository': {'i' + n: None if n == '99' else {'number': int(n)} for n in numbers}}})); sys.exit(0)
if os.environ.get('GH_FAIL'):
    print(json.dumps({'errors': [{'message': 'denied'}], 'data': {}})); sys.exit(0)
if os.environ.get('GH_EXIT'):
    print('offline', file=sys.stderr); sys.exit(1)
if os.environ.get('GH_INVALID'):
    print('not json'); sys.exit(0)
n = int(args['number']); connection = args['connection']; cursor = args.get('cursor', '')
if os.environ.get('GH_MISSING') and n == 4:
    print(json.dumps({'data': {'repository': {'issue': None}}})); sys.exit(0)
children = {1044: ([2] if not cursor else [3]), 2: [4], 3: [], 4: []}
blockers = {1044: [], 2: [], 3: ([2] if not cursor else [4]), 4: []}
nums = (children if connection == 'subIssues' else blockers).get(n, [])
node = lambda x: {'number': x, 'repository': {'nameWithOwner': 'Example/Repo'}}
more = ((n == 1044 and connection == 'subIssues') or (n == 3 and connection == 'blockedBy')) and (not cursor or bool(os.environ.get('GH_REPEAT')))
if os.environ.get('GH_REPEAT') and cursor:
    nums = []
count = 2 if (n == 1044 and connection == 'subIssues') or (n == 3 and connection == 'blockedBy') else len(nums)
if os.environ.get('GH_INCOMPLETE') and n == 4 and connection == 'blockedBy':
    count += 1
conn = {'nodes': [node(x) for x in nums], 'totalCount': count, 'pageInfo': {'hasNextPage': more, 'endCursor': 'page2' if more else None}}
issue = {'number': n, 'title': str(n), 'state': 'CLOSED' if n in (2, 7) else 'OPEN', 'repository': {'nameWithOwner': 'example/repo'}, connection: conn}
print(json.dumps({'data': {'repository': {'issue': issue}}}))
''')
        self.fake.chmod(0o755)

    def run_cli(self, *, use_historical_override=True, **env):
        args = [sys.executable, str(CLI), '--root', str(self.root), '--repo', 'example/repo']
        if use_historical_override:
            args += ['--historical-ledger', str(self.historical)]
        return subprocess.run(args, text=True, capture_output=True, env={**os.environ, 'PATH': str(self.root) + os.pathsep + os.environ['PATH'], **env})

    def test_join_reports_rows_and_nested_paginated_graph(self):
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['rows']['r2']['deliveries'][0]['id'], 'receipt')
        self.assertEqual(report['rows']['r2']['mapping']['history'], True)
        self.assertEqual(report['rows']['r1']['route_batches'][0]['id'], 'B1')
        self.assertEqual(report['unreferenced']['owner'], {'null': ['r1', 'r4'], 'draft': ['r3']})
        self.assertEqual(report['historical_unreferenced']['owner'], {'null': ['r1'], 'draft': ['r3']})
        self.assertEqual(report['rollup']['owner']['implementation'], {'implemented': 1, 'missing': 2, 'partial': 1})
        self.assertEqual(report['closed_ticket_successors']['insufficient_successor_information']['example/repo#2']['rows'], ['r2'])
        self.assertIn('missing.md', [item['target'] for item in report['reference_lint']['broken_relative_links']])
        self.assertIn('example/repo#99', report['reference_lint']['missing_tracker_references'])
        self.assertEqual(report['graph']['example/repo#3']['blocked_by'], ['example/repo#2', 'example/repo#4'])
        self.assertIn('example/repo#4', report['graph'])
        self.assertNotIn('example/repo#3', report['open_tickets_without_rows'])
        self.assertNotIn('example/repo#4', report['open_tickets_without_rows'])
        self.assertIn('example/repo#5', report['open_tickets_without_rows'])
        self.assertEqual(len(report['historical_group_table']['groups']), 17)
        self.assertFalse(report['historical_group_table']['groups'][8]['selection_complete'])

    def test_successor_evidence_and_completeness(self):
        source = 'docs/wiki/pi-alignment/decisions-2026-11-04.md'
        def run_with(successors, complete):
            self.mapping.write_text(json.dumps({'version': 1, 'scope': 'fixture', 'entries': [
                {'ticket': '#2', 'successors': successors, 'complete': complete, 'source': source}]}))
            result = self.run_cli()
            self.assertEqual(result.returncode, 0, result.stderr)
            return json.loads(result.stdout)['closed_ticket_successors']
        known = run_with(['#6'], False)['known_open_documented_successor']['example/repo#2']
        self.assertEqual(known['open_successors'], ['example/repo#6'])
        self.assertIn('example/repo#2', run_with(['#7'], True)['checked_no_documented_open_successor'])
        self.assertIn('example/repo#2', run_with(['#7'], False)['insufficient_successor_information'])
        self.mapping.write_text(json.dumps({'version': 1, 'scope': 'fixture', 'entries': [
            {'ticket': '#2', 'successors': [], 'complete': True, 'source': 'missing.md'}]}))
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('missing successor source', result.stderr)

    def test_candidate_snapshot_compares_each_group(self):
        candidate = self.HISTORICAL_LEDGER.read_bytes()
        # The fixture is the frozen git object, not a re-serialized copy: the
        # assertion below fails if the bytes drift from blob b550370.
        blob = hashlib.sha1(b'blob %d\0' % len(candidate) + candidate).hexdigest()
        self.assertEqual(blob, self.HISTORICAL_LEDGER_BLOB,
                         'the frozen ledger fixture is not the a4df1a115 blob')
        self.historical.write_bytes(candidate)
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        historical = json.loads(result.stdout)['historical_group_table']
        self.assertEqual(historical['population_count'], 341)
        groups = historical['groups']
        for number in (3, 6, 7, 8, 10, 17):
            self.assertEqual(groups[number - 1]['count_delta'], 0)
        self.assertEqual(len(groups[8]['named_rows_outside_population']), 3)
        self.assertFalse(groups[15]['selection_complete'])
        self.assertIsNone(groups[15]['count_delta'])
        self.assertFalse(groups[1]['selection_complete'])
        self.assertIn('cch_durable:abort-cascade-bottom-up', groups[1]['candidate_rows'])
        self.assertIn('cch_durable:abort-task-protocol', groups[1]['candidate_rows'])
        self.assertIn('cch_durable:Interchange:cpp-create', groups[0]['candidate_rows'])
        self.assertIn('cch_durable:steering-followup', groups[4]['candidate_rows'])
        self.assertIn('cch_coding_agent:Session:assembly:createAgentSession',
                      [row['id'] for row in groups[10]['named_rows_outside_population']])
        self.assertIsNone(historical['candidate_snapshot'])

    def test_named_group_nine_rows_inside_the_caller_population(self):
        candidate = json.loads(self.historical.read_text())
        candidate['rows'].append({'id': 'cch_codemode:codemode-remote', 'owner': 'cch_codemode',
                                  'membership': 'supported', 'implementation': 'partial',
                                  'verification': 'unverified', 'implementation_ticket': None})
        self.historical.write_text(json.dumps(candidate))
        result = self.run_cli()
        self.assertEqual(result.returncode, 0, result.stderr)
        group = json.loads(result.stdout)['historical_group_table']['groups'][8]
        self.assertIn('cch_codemode:codemode-remote', group['candidate_rows'])
        self.assertNotIn('cch_codemode:codemode-remote',
                         [row['id'] for row in group['named_rows_outside_population']])
        self.assertNotIn('are implemented and outside', group['boundary'])

    def test_reordered_historical_groups_fail_closed(self):
        proposal = self.root / 'docs/wiki/pi-alignment/reconciliation-1183-proposal-2026-11-04.md'
        text = proposal.read_text()
        lines = text.splitlines()
        start = next(i for i, line in enumerate(lines) if line.startswith('## 2.'))
        indexes = [i for i in range(start, len(lines)) if lines[i].startswith('|')][2:]
        lines[indexes[2]], lines[indexes[3]] = lines[indexes[3]], lines[indexes[2]]
        proposal.write_text('\n'.join(lines))
        result = self.run_cli()
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout, '')
        self.assertIn('historical group identities', result.stderr)

    def test_default_historical_git_input_and_missing_object(self):
        fake_git = self.root / 'git'
        fake_git.write_text(f'#!{sys.executable}\n' + '''import os, sys
if os.environ.get('GIT_MISSING'):
    print('missing candidate object', file=sys.stderr); sys.exit(1)
with open(os.environ['HISTORICAL_FIXTURE']) as source:
    print(source.read())
''')
        fake_git.chmod(0o755)
        result = self.run_cli(use_historical_override=False, HISTORICAL_FIXTURE=str(self.historical))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['historical_group_table']['candidate_snapshot'],
                         'a4df1a1153b044f3e406cf543d36f3ad1180c864')
        missing = self.run_cli(use_historical_override=False, GIT_MISSING='1')
        self.assertNotEqual(missing.returncode, 0)
        self.assertEqual(missing.stdout, '')

    def test_failures_never_emit_a_partial_report(self):
        for mode, message in [('GH_FAIL', 'denied'), ('GH_EXIT', 'offline'),
                              ('GH_INVALID', 'Expecting value'), ('GH_MISSING', 'missing or mismatched'),
                              ('GH_REPEAT', 'repeated cursor'), ('GH_INCOMPLETE', 'incomplete'),
                              ('GH_LINT_FAIL', 'lint denied')]:
            with self.subTest(mode=mode):
                result = self.run_cli(**{mode: '1'})
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, '')
                self.assertIn(message, result.stderr)


if __name__ == '__main__':
    unittest.main()
