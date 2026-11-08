#!/usr/bin/env python3
"""Public CLI checks with a fixed ledger and a fake GitHub boundary."""
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
        (wiki / 'reconciliation-1183-proposal-2026-11-04.md').write_text('## 2. Classification\n\n| Group | Rows | Proposed owner |\n| --- | --- | --- |\n| Example | 1 | #2 |\n\n## 3. Next\n[broken](missing.md) #99\n')
        (wiki / 'decisions-2026-11-04.md').write_text('| N5 → B1 | [#3](https://github.com/example/repo/issues/3) | TUI batch |\n')
        self.fake = self.root / 'gh'
        self.fake.write_text(f'#!{sys.executable}\n' + '''import json, os, sys
args = dict(x.split('=', 1) for x in sys.argv if '=' in x)
if args.get('lint') == '1':
    if os.environ.get('GH_LINT_FAIL'):
        print(json.dumps({'errors': [{'message': 'lint denied'}], 'data': {}})); sys.exit(0)
    print(json.dumps({'data': {'repository': {'i99': None}}})); sys.exit(0)
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
issue = {'number': n, 'title': str(n), 'state': 'CLOSED' if n == 2 else 'OPEN', 'repository': {'nameWithOwner': 'example/repo'}, connection: conn}
print(json.dumps({'data': {'repository': {'issue': issue}}}))
''')
        self.fake.chmod(0o755)

    def run_cli(self, **env):
        return subprocess.run([sys.executable, str(CLI), '--root', str(self.root), '--repo', 'example/repo'], text=True, capture_output=True, env={**os.environ, 'PATH': str(self.root) + os.pathsep + os.environ['PATH'], **env})

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
        self.assertEqual(report['closed_tickets_successor_unknown']['example/repo#2'], ['r2'])
        self.assertEqual(report['reference_lint']['broken_relative_links'][0]['target'], 'missing.md')
        self.assertIn('example/repo#99', report['reference_lint']['missing_tracker_references'])
        self.assertEqual(report['graph']['example/repo#3']['blocked_by'], ['example/repo#2', 'example/repo#4'])
        self.assertIn('example/repo#4', report['graph'])
        self.assertNotIn('example/repo#3', report['open_tickets_without_rows'])
        self.assertNotIn('example/repo#4', report['open_tickets_without_rows'])
        self.assertIn('example/repo#5', report['open_tickets_without_rows'])
        self.assertIn('Example', report['historical_group_table']['table'])

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
