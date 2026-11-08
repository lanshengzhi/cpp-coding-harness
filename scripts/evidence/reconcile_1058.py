#!/usr/bin/env python3
"""Read-only ledger and live GitHub issue graph reconciliation (#1208)."""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
from urllib.parse import unquote, urlsplit

DEFAULT_ROOT = Path(__file__).resolve().parents[2]
FILES = {
    'ledger': 'docs/research/pi-v1.3.0/1058-consolidated-capability-ledger.json',
    'deliveries': 'docs/research/pi-v1.3.0/1058-deliveries.json',
    'frontier': 'docs/research/pi-v1.3.0/1058-implementation-frontier.json',
    'plan': 'fixtures/pi-v1.3.0/consolidation-1058/consolidation-plan.json',
    'routes': 'docs/research/pi-v1.3.0/1182-tui-route-batches.json',
}


def reference(value, repo):
    if value is None or value == '#1058-draft':
        return None
    if not isinstance(value, str) or not re.fullmatch(r'#[1-9][0-9]*', value):
        raise ValueError(f'invalid ticket reference: {value!r}')
    return f'{repo}{value}'


def connection(repo, number, name):
    owner, repository = repo.split('/')
    query = '''query($owner: String!, $repo: String!, $number: Int!, $cursor: String) {
      repository(owner: $owner, name: $repo) { issue(number: $number) {
        number title state repository { nameWithOwner }
        CONNECTION(first: 100, after: $cursor) {
          totalCount nodes { number repository { nameWithOwner } }
          pageInfo { hasNextPage endCursor }
        }
      } }
    }'''.replace('CONNECTION', name)
    nodes, seen, cursors, cursor, metadata, expected = [], set(), set(), None, None, None
    while True:
        cmd = ['gh', 'api', 'graphql', '-f', 'query=' + query, '-f', 'owner=' + owner,
               '-f', 'repo=' + repository, '-F', 'number=' + str(number), '-f', 'connection=' + name]
        if cursor is not None:
            cmd += ['-f', 'cursor=' + cursor]
        proc = subprocess.run(cmd, text=True, capture_output=True, check=False, timeout=30)
        if proc.returncode:
            raise ValueError(f'gh failed for {repo}#{number} {name}: {proc.stderr.strip()}')
        result = json.loads(proc.stdout)
        if result.get('errors'):
            raise ValueError(f'GraphQL errors: {result["errors"]}')
        issue = result['data']['repository']['issue']
        if not issue or issue['number'] != number or issue['repository']['nameWithOwner'].lower() != repo.lower():
            raise ValueError(f'missing or mismatched issue {repo}#{number}')
        current = (issue['title'], issue['state'])
        if not isinstance(current[0], str) or current[1] not in ('OPEN', 'CLOSED'):
            raise ValueError(f'invalid issue metadata: {repo}#{number}')
        if metadata is not None and metadata != current:
            raise ValueError(f'issue changed during pagination: {repo}#{number}')
        metadata = current
        page = issue[name]
        count = page['totalCount']
        if expected is not None and count != expected:
            raise ValueError(f'connection changed during pagination: {repo}#{number} {name}')
        expected = count
        if not isinstance(count, int) or isinstance(count, bool) or count < 0 or not isinstance(page['nodes'], list):
            raise ValueError(f'invalid connection for {repo}#{number} {name}')
        for node in page['nodes']:
            node_repo, node_number = node['repository']['nameWithOwner'], node['number']
            if not isinstance(node_repo, str) or not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', node_repo) or not isinstance(node_number, int) or isinstance(node_number, bool) or node_number < 1:
                raise ValueError(f'invalid graph node in {repo}#{number} {name}')
            key = f'{node_repo.lower()}#{node_number}'
            if key in seen:
                raise ValueError(f'duplicate graph node {key} in {repo}#{number} {name}')
            seen.add(key)
            nodes.append(key)
        info = page['pageInfo']
        if not isinstance(info['hasNextPage'], bool):
            raise ValueError('invalid pageInfo')
        if not info['hasNextPage']:
            if len(nodes) != expected:
                raise ValueError(f'incomplete {name} for {repo}#{number}: {len(nodes)}/{expected}')
            return metadata, sorted(nodes)
        next_cursor = info['endCursor']
        if not isinstance(next_cursor, str) or not next_cursor or next_cursor == cursor or next_cursor in cursors:
            raise ValueError(f'invalid or repeated cursor for {repo}#{number} {name}')
        cursors.add(next_cursor)
        cursor = next_cursor


def graph(repo, referenced):
    pending = {f'{repo}#1044'}
    hierarchy, issues = set(), {}
    with ThreadPoolExecutor(max_workers=8) as executor:
        while pending:
            batch = sorted(pending - hierarchy)
            if not batch:
                break
            hierarchy.update(batch)
            missing = [key for key in batch if key not in issues]
            issues.update(zip(missing, executor.map(read_issue, missing)))
            pending = {child for key in batch for child in issues[key]['sub_issues']}
        missing = sorted(referenced - issues.keys())
        issues.update(zip(missing, executor.map(read_issue, missing)))
    return dict(sorted(issues.items())), hierarchy


def read_issue(key):
    repo, number = key.rsplit('#', 1)
    a, children = connection(repo, int(number), 'subIssues')
    b, blockers = connection(repo, int(number), 'blockedBy')
    if a != b:
        raise ValueError(f'issue changed while reading connections: {key}')
    return {'title': a[0], 'state': a[1], 'sub_issues': children, 'blocked_by': blockers}


def wiki_lint(root, graph_keys, repo):
    wiki = root / 'docs/wiki/pi-alignment'
    broken, tracker = [], set()
    for path in sorted(wiki.glob('*.md')):
        text = path.read_text()
        for target in re.findall(r'(?<!!)\[[^\]]*\]\(([^\s)]+)', text):
            parts = urlsplit(target)
            if not parts.scheme and parts.path and not (path.parent / unquote(parts.path)).exists():
                broken.append({'file': str(path.relative_to(root)), 'target': target})
        tracker.update(f'{repo}#{n}' for n in re.findall(r'(?<![\w#])#([1-9][0-9]*)\b', text))
    unchecked = sorted(tracker - set(graph_keys))
    missing = []
    owner, name = repo.split('/')
    for offset in range(0, len(unchecked), 50):
        batch = unchecked[offset:offset + 50]
        numbers = [int(key.rsplit('#', 1)[1]) for key in batch]
        fields = ' '.join(f'i{number}: issue(number: {number}) {{ number }}' for number in numbers)
        query = 'query($owner: String!, $repo: String!) { repository(owner: $owner, name: $repo) { ' + fields + ' } }'
        proc = subprocess.run(['gh', 'api', 'graphql', '-f', 'query=' + query, '-f', 'owner=' + owner,
                               '-f', 'repo=' + name, '-f', 'lint=1'],
                              text=True, capture_output=True, check=False, timeout=30)
        if proc.returncode:
            raise ValueError(f'gh failed for tracker lint: {proc.stderr.strip()}')
        result = json.loads(proc.stdout)
        if result.get('errors'):
            raise ValueError(f'GraphQL errors: {result["errors"]}')
        repository = result['data']['repository']
        if repository is None:
            raise ValueError(f'missing repository {repo}')
        for key, number in zip(batch, numbers):
            issue = repository[f'i{number}']
            if issue is None:
                missing.append(key)
            elif issue['number'] != number:
                raise ValueError(f'mismatched tracker reference {key}')
    return {'scope': 'Markdown links and #number references in docs/wiki/pi-alignment/*.md',
            'broken_relative_links': broken, 'missing_tracker_references': missing}


def parse_batch_tickets(decisions, repo):
    tickets = {}
    for batch_id, number in re.findall(r'\| N5 → (B[0-9]+) \| \[#([1-9][0-9]*)\]', decisions):
        if batch_id in tickets:
            raise ValueError(f'duplicate route batch mapping {batch_id}')
        tickets[batch_id] = f'{repo}#{number}'
    return tickets


def historical_group_table(proposal):
    section = proposal.split('## 2.', 1)[1].split('## 3.', 1)[0]
    lines = [line for line in section.splitlines() if line.startswith('|')]
    if len(lines) < 3:
        raise ValueError('proposal group table missing')
    return '\n'.join(lines)


def successor_mapping(root, repo):
    path = root / 'scripts/evidence/1058-successors.json'
    data = json.loads(path.read_text())
    if data['version'] != 1 or not isinstance(data['scope'], str):
        raise ValueError('invalid successor mapping version or scope')
    entries, refs = {}, set()
    for entry in data['entries']:
        key = reference(entry['ticket'], repo)
        successors = [reference(value, repo) for value in entry['successors']]
        if (not key or key in entries or None in successors or key in successors
                or len(set(successors)) != len(successors) or not isinstance(entry['complete'], bool)
                or not isinstance(entry['source'], str) or not entry['source'].strip()):
            raise ValueError(f'invalid successor entry for {key}')
        source_path = entry['source'].split('#', 1)[0]
        if not source_path or Path(source_path).is_absolute() or '..' in Path(source_path).parts or not (root / source_path).is_file():
            raise ValueError(f'missing successor source: {entry["source"]}')
        entries[key] = {'successors': successors, 'complete': entry['complete'], 'source': entry['source']}
        refs.update(successors)
    return entries, refs


def historical_comparison(proposal, ledger):
    table = historical_group_table(proposal)
    lines = table.splitlines()[2:]
    descriptions = '\n'.join(line.strip('|').split('|')[0].strip() for line in lines)
    expected = 'cb30a8e73fd304f84c9bb13d27b5af7aaa924c7db0e634b6d0440d380bc97c1a'
    if len(lines) != 17 or hashlib.sha256(descriptions.encode()).hexdigest() != expected:
        raise ValueError('historical group identities or order changed')
    groups = []
    population = [r for r in ledger['rows'] if r['membership'] == 'supported'
                  and r['implementation'] in ('missing', 'partial')
                  and r['implementation_ticket'] in (None, '#1058-draft')]
    # Select only descriptions that identify rows unambiguously; partial selections
    # are reported as such, never presented as a reconstructed full partition.
    owners = {10: 'cch_agent_core', 16: 'cch_tui', 17: 'cch_mcp'}
    named = {
        2: ('cch_durable', ('task-reservation-checkpoint', 'task-terminal-receipt-atomicity', 'atomic-admission-inbox', 'request-id-deduplication')),
        3: ('cch_durable', ('node-env-watch', 'node-exec', 'node-filesystem')),
        4: ('cch_durable', ('codemode-script', 'codemode-replay', 'codemode-failure', 'event-stream', 'abandon-on-restart', 'Storage:jsonl:fsync')),
        5: ('cch_durable', ('background-compaction', 'idle-waits', 'submission-queues', 'harness-settings-defaults', 'native-run-triggering', 'codemode-models', 'codemode-output', 'codemode-limits')),
        6: ('cch_chord', ('chord-delta', 'chord-documents')),
        7: ('cch_chord', ('context:root-background', 'json:json-representation', 'output:progress', 'Chord:context')),
        8: ('cch_codemode', ('codemode-execute-outcome', 'codemode-inline-close', 'codemode-portable-entry', 'codemode-script-clock-timezone')),
        9: ('cch_codemode', ('codemode-remote', 'codemode-sandbox-create-errors', 'codemode-source-output-token-option')),
        11: ('cch_coding_agent', ('Session:assembly:createAgentSession',)),
        13: ('cch_coding_agent', ('mcp-external-signin-pickup', 'mcp-management-ui')),
        15: ('cch_coding_agent', ('osc52-opt-in', 'tool-read-line-range', 'system-theme', 'residual-1032-system-theme')),
    }
    reasons = {
        1: 'Draft Document/Entry/Identity/Query/Session/Storage/Tx and Interchange rows are candidates; unpublished overlapping group assignments prevent a full partition.',
        2: 'Names four rows and abort-*; the proposal later records ambiguity in the 11-row selection.',
        3: 'All three exact names are published.',
        4: 'Named examples include codemode rows; codemode-* overlaps the unobserved group and assignment precedence is unpublished.',
        5: 'Examples overlap group 4 and no complete assignment or precedence is published.',
        6: 'The proposal names all document:* rows and both coarse rows.',
        7: 'All four exact names are published.',
        8: 'All four exact names are published.',
        9: 'Compare the three #1193 source-only rows named in §1.5 against the supplied population; the published table describes them as implemented.',
        10: 'The candidate population has only these agent-core rows.',
        11: 'Candidate Session:* draft rows follow the description; group overlap and complete membership remain unpublished.',
        12: 'Subject descriptions overlap other groups; complete membership is unpublished.',
        13: 'MCP description includes a withdrawn row that is not missing/partial; complete membership is unpublished.',
        14: 'Named residual prefixes yield candidates, but overlap the unobserved group; precedence and exact membership are unpublished.',
        15: 'Four exact names are published; one is also in the residual group.',
        16: 'The candidate population has only these TUI rows; proposal mentions terminal rows but their candidate ticket state must be checked.',
        17: 'Both exact names and the owner population are published.',
    }
    for index, line in enumerate(lines, 1):
        cells = [cell.strip() for cell in line.strip('|').split('|')]
        if len(cells) != 3 or not cells[1].isdigit():
            raise ValueError(f'invalid proposal group {index}')
        selected = []
        for row in population:
            owner = row['owner']
            suffix = row['id'].removeprefix(owner + ':')
            include = owner == owners.get(index)
            if index in named:
                target, names = named[index]
                include = owner == target and (suffix in names or (index == 6 and suffix.startswith('document:')))
            if index == 2 and owner == 'cch_durable' and suffix.startswith('abort-'):
                include = True
            if index == 1:
                include = (owner == 'cch_durable' and
                           ((row['implementation_ticket'] == '#1058-draft'
                             and suffix.startswith(('Document:', 'Entry:', 'Identity:', 'Query:', 'Session:', 'Storage:', 'Tx:')))
                            or suffix.startswith('Interchange:')))
            if index == 5 and owner == 'cch_durable' and suffix.startswith('steering-'):
                include = True
            if index == 11:
                include = owner == 'cch_coding_agent' and row['implementation_ticket'] == '#1058-draft' and suffix.startswith('Session:')
            if index == 14:
                include = owner == 'cch_coding_agent' and suffix.startswith(('residual-808-', 'residual-810-', 'residual-812-', 'residual-1032-', 'residual-1043-'))
            if include:
                selected.append(row['id'])
        outside = []
        if index == 9:
            names = {'cch_codemode:codemode-remote', 'cch_codemode:codemode-sandbox-create-errors',
                     'cch_codemode:codemode-source-output-token-option'}
            outside = [{'id': r['id'], 'implementation': r['implementation'], 'ticket': r['implementation_ticket']}
                       for r in ledger['rows'] if r['id'] in names and r not in population]
        if index == 11:
            outside = [{'id': r['id'], 'implementation': r['implementation'], 'ticket': r['implementation_ticket']}
                       for r in ledger['rows'] if r['id'] == 'cch_coding_agent:Session:assembly:createAgentSession' and r not in population]
        if index == 13:
            outside = [{'id': r['id'], 'implementation': r['implementation'], 'ticket': r['implementation_ticket']}
                       for r in ledger['rows'] if r['id'] == 'cch_coding_agent:mcp-management-ui' and r not in population]
        complete = index in (3, 6, 7, 8, 10, 17)
        groups.append({'group': index, 'description': cells[0], 'published_count': int(cells[1]),
                       'candidate_rows': sorted(selected), 'candidate_count': len(selected), 'named_rows_outside_population': outside,
                       'selection_complete': complete, 'count_delta': len(selected) - int(cells[1]) if complete else None,
                       'boundary': reasons.get(index, 'No complete row-level assignment or precedence is published.')})
    if len(groups) != 17:
        raise ValueError(f'expected 17 historical groups, got {len(groups)}')
    return {'candidate_snapshot': 'a4df1a1153b044f3e406cf543d36f3ad1180c864',
            'provenance': 'Candidate ledger; proposal used an uncommitted tree and live issue state, neither fully preserved. Counts alone do not establish membership parity.',
            'population_count': len(population), 'groups': groups, 'table': table}


def report(root, repo, historical_ledger):
    data = {name: json.loads((root / path).read_text()) for name, path in FILES.items()}
    rows = {}
    unreferenced = defaultdict(lambda: {'null': [], 'draft': []})
    historical_unreferenced = defaultdict(lambda: {'null': [], 'draft': []})
    rollup = defaultdict(lambda: {'implementation': Counter(), 'verification': Counter(), 'membership': Counter()})
    refs = set()
    for row in data['ledger']['rows']:
        rid, owner, ticket = row['id'], row['owner'], row['implementation_ticket']
        if rid in rows:
            raise ValueError(f'duplicate ledger row {rid}')
        key = reference(ticket, repo)
        if key:
            refs.add(key)
        category = 'null' if ticket is None else 'draft' if ticket == '#1058-draft' else None
        if category:
            unreferenced[owner][category].append(rid)
            if row['membership'] == 'supported' and row['implementation'] in ('missing', 'partial'):
                historical_unreferenced[owner][category].append(rid)
        for field in ('membership', 'implementation', 'verification'):
            rollup[owner][field][row[field]] += 1
        rows[rid] = {'owner': owner, 'membership': row['membership'], 'implementation': row['implementation'],
                     'verification': row['verification'], 'ticket': ticket, 'mapping': None, 'deliveries': [], 'route_batches': []}
    for entry in data['plan'].get('ticketMappings', {}).get('entries', []):
        mapping_ticket = reference(entry['ticket'], repo)
        if not mapping_ticket:
            raise ValueError(f'invalid mapping ticket: {entry["ticket"]!r}')
        refs.add(mapping_ticket)
        for rid in entry['rows']:
            if rid not in rows or rows[rid]['mapping'] is not None:
                raise ValueError(f'unknown or duplicate mapping row {rid}')
            rows[rid]['mapping'] = {k: entry[k] for k in ('id', 'ticket', 'history', 'note') if k in entry}
    for receipt in data['deliveries']['records']:
        ticket = receipt['ticket']
        if not isinstance(ticket, str) or not re.fullmatch(r'[1-9][0-9]*', ticket):
            raise ValueError(f'invalid delivery ticket: {ticket!r}')
        refs.add(f'{repo}#{ticket}')
        for item in receipt['rows']:
            rid = item['id']
            if rid not in rows:
                raise ValueError(f'unknown delivery row {rid}')
            rows[rid]['deliveries'].append({'id': receipt['id'], 'ticket': receipt['ticket']})
    decisions = (root / 'docs/wiki/pi-alignment/decisions-2026-11-04.md').read_text()
    batch_tickets = parse_batch_tickets(decisions, repo)
    route_rows = defaultdict(list)
    for batch in data['routes']['batches']:
        if batch['id'] not in batch_tickets:
            raise ValueError(f'missing route ticket mapping for {batch["id"]}')
        key = batch_tickets[batch['id']]
        refs.add(key)
        for rid in batch['rows']:
            if rid not in rows:
                raise ValueError(f'unknown route row {rid}')
            route_rows[key].append(rid)
            rows[rid]['route_batches'].append({**{k: batch[k] for k in ('id', 'owner', 'dependency')}, 'ticket': key})
    for item in data['frontier']['residualReconciliation']:
        key = reference(item['issue'], repo)
        if key:
            refs.add(key)
    successors, successor_refs = successor_mapping(root, repo)
    refs.update(successor_refs)
    issues, _hierarchy = graph(repo, refs)
    by_ticket = defaultdict(list)
    for rid, row in rows.items():
        key = reference(row['ticket'], repo)
        if key:
            by_ticket[key].append(rid)
    associated_tickets = set(by_ticket) | set(route_rows)
    for row in rows.values():
        if row['mapping']:
            associated_tickets.add(reference(row['mapping']['ticket'], repo))
        associated_tickets.update(f'{repo}#{delivery["ticket"]}' for delivery in row['deliveries'])
    proposal = (root / 'docs/wiki/pi-alignment/reconciliation-1183-proposal-2026-11-04.md').read_text()
    historical = historical_comparison(proposal, historical_ledger)
    closed = {'known_open_documented_successor': {}, 'checked_no_documented_open_successor': {},
              'insufficient_successor_information': {}}
    for key, ids in sorted(by_ticket.items()):
        if issues[key]['state'] != 'CLOSED':
            continue
        entry = successors.get(key)
        open_successors = sorted(s for s in entry['successors'] if issues[s]['state'] == 'OPEN') if entry else []
        category = ('known_open_documented_successor' if open_successors else
                    'checked_no_documented_open_successor' if entry and entry['complete'] else
                    'insufficient_successor_information')
        closed[category][key] = {'rows': sorted(ids), 'open_successors': open_successors,
                                 'evidence': entry}
    return {'rows': dict(sorted(rows.items())), 'unreferenced': dict(sorted(unreferenced.items())),
            'historical_unreferenced': dict(sorted(historical_unreferenced.items())),
            'rollup': dict(sorted(rollup.items())), 'graph': issues,
            'frontier': {'owners': data['frontier']['ownerGraph']['nodes'],
                         'residual_reconciliation': data['frontier']['residualReconciliation']},
            'closed_ticket_successors': closed,
            'open_tickets_without_rows': sorted(key for key in issues if issues[key]['state'] == 'OPEN' and key not in associated_tickets),
            'reference_lint': wiki_lint(root, issues, repo),
            'historical_group_table': historical}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=DEFAULT_ROOT, help='input checkout root')
    parser.add_argument('--repo', default='lanshengzhi/cpp-coding-harness', help='GitHub owner/repository')
    parser.add_argument('--historical-ledger', type=Path, help='candidate ledger override; default requires Git object a4df1a115')
    args = parser.parse_args()
    try:
        if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', args.repo):
            raise ValueError('invalid repository')
        if args.historical_ledger:
            historical_ledger = json.loads(args.historical_ledger.read_text())
        else:
            proc = subprocess.run(['git', 'show', 'a4df1a1153b044f3e406cf543d36f3ad1180c864:' + FILES['ledger']],
                                  cwd=DEFAULT_ROOT, text=True, capture_output=True, check=True, timeout=30)
            historical_ledger = json.loads(proc.stdout)
        result = report(args.root, args.repo.lower(), historical_ledger)
        if args.historical_ledger:
            result['historical_group_table']['candidate_snapshot'] = None
            result['historical_group_table']['provenance'] = f'Caller-supplied ledger: {args.historical_ledger}; snapshot identity not verified.'
        print(json.dumps(result, indent=2, sort_keys=True))
    except (OSError, ValueError, KeyError, TypeError, IndexError, subprocess.TimeoutExpired, subprocess.CalledProcessError) as exc:
        print(f'reconciliation failed: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
