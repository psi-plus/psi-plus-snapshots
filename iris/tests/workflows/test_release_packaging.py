"""Exercise the release workflow's real shell selector with a fake GitHub API.

Run with: python3 -m unittest discover -s tests/workflows -v
Requires bash and jq; does not access the network.
"""

import json
import os
from pathlib import Path
import subprocess
import tempfile
import textwrap
import unittest


WORKFLOW = Path(__file__).resolve().parents[2] / ".github/workflows/release.yml"
SHA = "a" * 40
OTHER_SHA = "b" * 40

FAKE_GH = r'''#!/usr/bin/env python3
import json, os, re, sys
from pathlib import Path
args = sys.argv[1:]
runs = json.loads(Path(os.environ['TEST_RUNS']).read_text())
if args[:2] == ['run', 'list']:
    print(json.dumps(runs))
elif args[0] == 'api':
    run_id = int(re.search(r'/runs/(\d+)/', args[1])[1])
    run = next(run for run in runs if run['databaseId'] == run_id)
    if '--jq' not in args:
        print(json.dumps({'artifacts': run['artifacts']}))
        raise SystemExit(0)
    query = args[args.index('--jq') + 1]
    for artifact in run['artifacts']:
        if not artifact['expired']:
            if 'iris-package-source' in query:
                if artifact['name'] == 'iris-package-source':
                    print(artifact['id'])
            else:
                print(artifact['name'])
elif args[:2] == ['run', 'download']:
    run = next(run for run in runs if run['databaseId'] == int(args[2]))
    dest = Path(args[args.index('--dir') + 1])
    (dest / 'iris-package-source.json').write_text(json.dumps(run['source']))
elif args[:2] == ['run', 'view']:
    print('completed\tsuccess')
elif args[:2] == ['workflow', 'run']:
    new = json.loads(Path(os.environ['TEST_TEMPLATE']).read_text())
    new['databaseId'] = max([run['databaseId'] for run in runs], default=0) + 1
    runs.append(new)
    Path(os.environ['TEST_RUNS']).write_text(json.dumps(runs))
    Path(os.environ['TEST_DISPATCH']).write_text('dispatched')
else:
    raise SystemExit(f'Unexpected gh call: {args}')
'''


class PackagingSelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        workflow = WORKFLOW.read_text()
        step = workflow.split("      - name: Find or run packaging workflow\n", 1)[1]
        step = step.split("      - name: Download packaging artifact archives\n", 1)[0]
        script = textwrap.dedent(step.split("        run: |\n", 1)[1])
        cls.script = script
        cls.functions = script.split("\nload_runs\n", 1)[0]
        cls.names = script.split("expected_artifacts=(", 1)[1].split(")", 1)[0].split()

    def run_fixture(self, run_id=1, source_sha=SHA, version="1.1.2", status="completed", event="workflow_dispatch"):
        artifacts = [{"id": i, "name": name, "expired": False} for i, name in enumerate(self.names, 1)]
        artifacts.append({"id": 100, "name": "iris-package-source", "expired": False})
        return {
            "databaseId": run_id,
            "displayTitle": "Iris packages v1.1.2",
            "status": status,
            "conclusion": "success" if status == "completed" else "",
            "createdAt": f"2026-10-07T00:00:{run_id:02d}Z",
            "event": event,
            # A manual run executes a workflow commit distinct from its source.
            "headSha": OTHER_SHA,
            "source": {"source_sha": source_sha, "version": version},
            "artifacts": artifacts,
        }

    def select(self, runs, function="latest_successful_run", full=False, force=False):
        with tempfile.TemporaryDirectory(prefix="iris-release-test-") as temp:
            root = Path(temp)
            gh = root / "gh"
            gh.write_text(FAKE_GH)
            gh.chmod(0o755)
            fixtures = root / "runs.json"
            fixtures.write_text(json.dumps(runs))
            template = root / "template.json"
            template.write_text(json.dumps(self.run_fixture()))
            output = root / "output"
            dispatched = root / "dispatch"
            sleep = root / "sleep"
            sleep.write_text("#!/bin/sh\nexit 0\n")
            sleep.chmod(0o755)
            env = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}", TEST_RUNS=str(fixtures),
                       RUNNER_TEMP=temp, GITHUB_REPOSITORY="psi-im/iris", TAG="v1.1.2", SOURCE_SHA=SHA,
                       TEST_TEMPLATE=str(template), TEST_DISPATCH=str(dispatched), GITHUB_OUTPUT=str(output),
                       FORCE_REBUILD=str(force).lower())
            script = self.script if full else self.functions + f"\nload_runs\n{function}\n"
            result = subprocess.run(["bash", "-c", script],
                                    env=env, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr)
            if full:
                return output.read_text().strip(), dispatched.exists()
            return result.stdout.strip()

    def test_release_reuses_packages_without_dispatch(self):
        self.assertEqual(self.select([self.run_fixture()], full=True), ("run_id=1", False))

    def test_release_dispatches_when_source_differs(self):
        self.assertEqual(self.select([self.run_fixture(source_sha=OTHER_SHA)], full=True), ("run_id=2", True))

    def test_explicit_force_rebuild_dispatches_again(self):
        self.assertEqual(self.select([self.run_fixture()], full=True, force=True), ("run_id=2", True))

    def test_release_waits_for_matching_active_run(self):
        self.assertEqual(self.select([self.run_fixture(status="in_progress")], full=True), ("run_id=1", False))

    def test_manual_run_matches_actual_source_not_workflow_sha(self):
        self.assertEqual(self.select([self.run_fixture()]), "1")

    def test_wrong_source_rejected(self):
        self.assertEqual(self.select([self.run_fixture(source_sha=OTHER_SHA)]), "")

    def test_wrong_package_version_rejected(self):
        self.assertEqual(self.select([self.run_fixture(version="1.1.1")]), "")

    def test_missing_artifact_rejected(self):
        run = self.run_fixture()
        run["artifacts"].pop(0)
        self.assertEqual(self.select([run]), "")

    def test_previous_ubuntu_only_run_not_reusable(self):
        run = self.run_fixture()
        run["artifacts"] = [artifact for artifact in run["artifacts"]
                            if artifact["name"] != "iris-deb-debian-13"]
        self.assertEqual(self.select([run]), "")

    def test_expired_artifact_rejected(self):
        run = self.run_fixture()
        run["artifacts"][0]["expired"] = True
        self.assertEqual(self.select([run]), "")

    def test_searches_older_complete_run(self):
        incomplete = self.run_fixture(run_id=2)
        incomplete["artifacts"].pop(0)
        self.assertEqual(self.select([self.run_fixture(), incomplete]), "1")

    def test_newest_matching_complete_run_selected(self):
        self.assertEqual(self.select([self.run_fixture(), self.run_fixture(run_id=2)]), "2")

    def test_failed_run_not_selected(self):
        run = self.run_fixture()
        run["conclusion"] = "failure"
        self.assertEqual(self.select([run]), "")

    def test_expired_metadata_not_treated_as_legacy_push(self):
        run = self.run_fixture(event="push")
        run["headSha"] = SHA
        run["artifacts"][-1]["expired"] = True
        self.assertEqual(self.select([run]), "")

    def test_legacy_manual_run_is_unverifiable(self):
        run = self.run_fixture()
        run["headSha"] = SHA
        run["artifacts"].pop()
        self.assertEqual(self.select([run]), "")

    def test_legacy_tag_push_matches_event_sha(self):
        run = self.run_fixture(event="push")
        run["headSha"] = SHA
        run["artifacts"].pop()
        self.assertEqual(self.select([run]), "1")

    def test_legacy_wrong_event_sha_rejected(self):
        run = self.run_fixture(event="push")
        run["artifacts"].pop()
        self.assertEqual(self.select([run]), "")

    def test_active_matching_run_reused_before_packages_complete(self):
        run = self.run_fixture(status="in_progress")
        run["artifacts"] = [run["artifacts"][-1]]
        self.assertEqual(self.select([run], "latest_active_run"), "1")

    def test_active_wrong_source_rejected(self):
        self.assertEqual(self.select([self.run_fixture(status="in_progress", source_sha=OTHER_SHA)],
                                     "latest_active_run"), "")


if __name__ == "__main__":
    unittest.main()
