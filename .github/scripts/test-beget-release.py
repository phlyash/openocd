#!/usr/bin/env python3
"""Integration tests for the OpenOCD Beget incoming-bundle builder."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest import mock


ROOT = Path(__file__).resolve().parents[2]
BUILDER = ROOT / ".github" / "scripts" / "prepare-beget-release.py"
WORKFLOW = ROOT / ".github" / "workflows" / "build.yml"
FILES = {
    "openocd-linux-x86_64.tar.gz": ("linux", "x86_64", "tar.gz"),
    "openocd-linux-x86_64.zip": ("linux", "x86_64", "zip"),
    "openocd-macos-aarch64.tar.gz": ("darwin", "aarch64", "tar.gz"),
    "openocd-macos-aarch64.zip": ("darwin", "aarch64", "zip"),
    "openocd-windows-x86_64.tar.gz": ("windows", "x86_64", "tar.gz"),
    "openocd-windows-x86_64.zip": ("windows", "x86_64", "zip"),
}

EXPECTED_TEST_PUBLISHER_CLIENT_JOB = """  test-publisher-client:
    name: Test package publisher client
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4
      - run: python3 .github/scripts/test-beget-release.py
      - run: bash .github/scripts/test-deploy-beget.sh

"""

EXPECTED_DEPLOY_BEGET_JOB = """  deploy-beget:
    name: Publish release to Beget
    if: github.event_name == 'workflow_dispatch'
    needs: release
    runs-on: ubuntu-24.04
    steps:
      - uses: actions/checkout@v4

      - name: Download release artifacts
        uses: actions/download-artifact@v4
        with:
          pattern: openocd-*
          path: release-assets
          merge-multiple: true

      - name: Prepare Beget bundle
        env:
          RELEASE_TAG: ${{ inputs.release_tag }}
        run: >-
          python3 .github/scripts/prepare-beget-release.py
          --type openocd
          --tag "$RELEASE_TAG"
          --repository "$GITHUB_REPOSITORY"
          --run-id "$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT"
          --input release-assets
          --output beget-upload

      - name: Upload and publish on Beget
        env:
          BEGET_HOST: ${{ secrets.BEGET_HOST }}
          BEGET_PORT: ${{ secrets.BEGET_PORT }}
          BEGET_USER: ${{ secrets.BEGET_USER }}
          BEGET_SSH_PRIVATE_KEY: ${{ secrets.BEGET_SSH_PRIVATE_KEY }}
          BEGET_KNOWN_HOSTS: ${{ secrets.BEGET_KNOWN_HOSTS }}
        run: bash .github/scripts/deploy-beget-release.sh beget-upload
"""

EXPECTED_ON_SECTION = """on:
  push:
    branches: [master]
  pull_request:
  workflow_dispatch:
    inputs:
      build_name:
        description: Name appended to the openocd --version output
        required: true
        type: string
      release_tag:
        description: Git tag used for the published GitHub Release
        required: true
        type: string

"""

EXPECTED_PERMISSIONS_SECTION = """permissions:
  contents: read

"""

EXPECTED_RELEASE_NEEDS = """    needs:
      - macos-aarch64
      - linux-x86_64
      - windows-x86_64
"""


def job_block(workflow, name):
    """Extract only a same-indent job definition, excluding later jobs."""
    return re.search(
        rf"(?ms)^  {re.escape(name)}:\n.*?(?=^  [A-Za-z0-9_-]+:\n|\Z)", workflow
    )


def assert_job_contract(test_case, workflow, name, expected):
    """Require one exact, indentation-sensitive workflow job contract."""
    match = job_block(workflow, name)
    test_case.assertIsNotNone(match, f"{name} job is missing")
    test_case.assertEqual(match.group(0), expected)


def top_level_section(workflow, name):
    """Extract one zero-indent YAML section without consuming its neighbours."""
    return re.search(
        rf"(?ms)^{re.escape(name)}:\n.*?(?=^[A-Za-z0-9_-]+:\n|\Z)", workflow
    )


def assert_top_level_section_contract(test_case, workflow, name, expected):
    """Require one exact zero-indent YAML section contract."""
    match = top_level_section(workflow, name)
    test_case.assertIsNotNone(match, f"{name} section is missing")
    test_case.assertEqual(match.group(0), expected)


def job_level_field(job, name):
    """Extract one job-level YAML field through the following job-level field."""
    return re.search(
        rf"(?ms)^    {re.escape(name)}:\n.*?(?=^    [A-Za-z0-9_-]+:|\Z)", job
    )


def assert_complete_workflow_contract(test_case, workflow):
    """Assert the complete trigger, permission, fast-job, and deploy-job contract."""
    assert_top_level_section_contract(test_case, workflow, "on", EXPECTED_ON_SECTION)
    assert_top_level_section_contract(
        test_case, workflow, "permissions", EXPECTED_PERMISSIONS_SECTION
    )
    assert_job_contract(
        test_case, workflow, "test-publisher-client", EXPECTED_TEST_PUBLISHER_CLIENT_JOB
    )
    test_case.assertEqual(
        workflow.count("test-publisher-client"),
        1,
        "test-publisher-client must occur only in its own job declaration",
    )
    assert_job_contract(test_case, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)
    for name in ("macos-aarch64", "linux-x86_64", "windows-x86_64"):
        match = job_block(workflow, name)
        test_case.assertIsNotNone(match, f"{name} job is missing")
        test_case.assertNotRegex(match.group(0), r"(?m)^    needs:")
    release = job_block(workflow, "release")
    test_case.assertIsNotNone(release, "release job is missing")
    release_needs = job_level_field(release.group(0), "needs")
    test_case.assertIsNotNone(release_needs, "release needs field is missing")
    test_case.assertEqual(release_needs.group(0), EXPECTED_RELEASE_NEEDS)


def replace_in_job(workflow, name, old, new):
    """Return a fixture where one fragment changes inside one named job block."""
    match = job_block(workflow, name)
    if match is None:
        raise AssertionError(f"{name} job is missing from fixture")
    return workflow[:match.start()] + match.group(0).replace(old, new, 1) + workflow[match.end():]


def expected_summary(manifest):
    """Return the sanitized manifest-derived log lines required on success."""
    return [
        "Prepared bundle: type=%s version=%s" % (manifest["type"], manifest["version"]),
        *[
            "profile=%s/%s/%s sha256=%s"
            % (entry["os"], entry["arch"], entry["archiv"], entry["sha256"])
            for entry in manifest["files"]
        ],
    ]


class PrepareBegetReleaseTests(unittest.TestCase):
    """Tests for contract-visible bundle output and input rejection."""

    def make_input(self, directory):
        source = Path(directory) / "input"
        source.mkdir()
        for index, name in enumerate(FILES):
            (source / name).write_bytes(("archive-%d\\n" % index).encode() * 100)
        return source

    def run_builder(self, source, output, **overrides):
        options = {
            "type": "openocd",
            "tag": "v1.2.3",
            "repository": "phlyash/openocd",
            "run_id": "123456-1",
        }
        options.update(overrides)
        return subprocess.run(
            [
                sys.executable,
                str(BUILDER),
                "--type", options["type"],
                "--tag", options["tag"],
                "--repository", options["repository"],
                "--run-id", options["run_id"],
                "--input", str(source),
                "--output", str(output),
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def assert_failure_without_manifest(self, result, output):
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertFalse((output / "release.json").exists())

    def test_builds_ordered_manifest_and_byte_identical_archives(self):
        """A wrong mapping, hash, key order, or copy breaks the public contract."""
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            result = self.run_builder(source, output)

            self.assertEqual(result.returncode, 0, result.stderr)
            manifest = json.loads((output / "release.json").read_text())
            self.assertEqual(
                list(manifest),
                ["schema", "type", "version", "repository", "run_id", "files"],
            )
            self.assertEqual(manifest["schema"], 1)
            self.assertEqual(manifest["type"], "openocd")
            self.assertEqual(manifest["version"], "1.2.3")
            self.assertEqual(manifest["repository"], "phlyash/openocd")
            self.assertEqual(manifest["run_id"], "123456-1")
            self.assertEqual([entry["name"] for entry in manifest["files"]], list(FILES))
            self.assertEqual(
                [(entry["os"], entry["arch"], entry["archiv"]) for entry in manifest["files"]],
                list(FILES.values()),
            )
            for entry in manifest["files"]:
                self.assertEqual(list(entry), ["name", "os", "arch", "archiv", "sha256"])
                source_bytes = (source / entry["name"]).read_bytes()
                output_bytes = (output / entry["name"]).read_bytes()
                self.assertEqual(output_bytes, source_bytes)
                self.assertEqual(entry["sha256"], hashlib.sha256(output_bytes).hexdigest())
            self.assertEqual(sorted(path.name for path in output.iterdir()), sorted([*FILES, "release.json"]))
            self.assertEqual(result.stdout.splitlines(), expected_summary(manifest))

    def test_manifest_hash_describes_archive_copied_before_source_mutation(self):
        """A post-copy source change must not alter the completed bundle checksum."""
        specification = importlib.util.spec_from_file_location("prepare_beget_release", BUILDER)
        builder = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(builder)

        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            name = "openocd-linux-x86_64.tar.gz"
            mutated_contents = b"mutated after archive copy\\n"
            real_copyfile = builder.shutil.copyfile

            def copy_then_mutate(copy_source, destination):
                result = real_copyfile(copy_source, destination)
                if Path(copy_source).name == name:
                    Path(copy_source).write_bytes(mutated_contents)
                return result

            arguments = [
                str(BUILDER), "--type", "openocd", "--tag", "v1.2.3",
                "--repository", "phlyash/openocd", "--run-id", "123456-1",
                "--input", str(source), "--output", str(output),
            ]
            with mock.patch.object(sys, "argv", arguments):
                with mock.patch.object(builder.shutil, "copyfile", side_effect=copy_then_mutate):
                    builder.main()

            manifest = json.loads((output / "release.json").read_text())
            entry = next(entry for entry in manifest["files"] if entry["name"] == name)
            destination_hash = hashlib.sha256((output / name).read_bytes()).hexdigest()
            source_hash = hashlib.sha256((source / name).read_bytes()).hexdigest()
            self.assertEqual(entry["sha256"], destination_hash)
            self.assertNotEqual(entry["sha256"], source_hash)

    def test_manifest_replaces_its_own_temporary_file_only_after_serialization(self):
        """A manifest must atomically replace release.json from an output-local temp file."""
        specification = importlib.util.spec_from_file_location("prepare_beget_release", BUILDER)
        builder = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(builder)

        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            real_replace = builder.os.replace
            replacements = []

            def observe_replace(source_path, destination_path):
                replacements.append((Path(source_path), Path(destination_path)))
                return real_replace(source_path, destination_path)

            arguments = [
                str(BUILDER), "--type", "openocd", "--tag", "v1.2.3",
                "--repository", "phlyash/openocd", "--run-id", "123456-1",
                "--input", str(source), "--output", str(output),
            ]
            with mock.patch.object(sys, "argv", arguments):
                with mock.patch.object(builder.os, "replace", side_effect=observe_replace):
                    builder.main()

            self.assertEqual(len(replacements), 1)
            temporary_path, destination_path = replacements[0]
            self.assertEqual(temporary_path.parent, output)
            self.assertTrue(temporary_path.name.startswith(".release-"))
            self.assertEqual(destination_path, output / "release.json")
            self.assertFalse(temporary_path.exists())

    def test_serialization_failure_leaves_no_manifest_or_temporary_file(self):
        """A partially written manifest temp file must be removed when JSON output fails."""
        specification = importlib.util.spec_from_file_location("prepare_beget_release", BUILDER)
        builder = importlib.util.module_from_spec(specification)
        specification.loader.exec_module(builder)

        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"

            def write_partial_json_then_fail(_manifest, stream, **_kwargs):
                stream.write("{")
                raise OSError("simulated JSON serialization failure")

            arguments = [
                str(BUILDER), "--type", "openocd", "--tag", "v1.2.3",
                "--repository", "phlyash/openocd", "--run-id", "123456-1",
                "--input", str(source), "--output", str(output),
            ]
            with mock.patch.object(sys, "argv", arguments):
                with mock.patch.object(builder.json, "dump", side_effect=write_partial_json_then_fail):
                    with self.assertRaisesRegex(OSError, "simulated JSON serialization failure"):
                        builder.main()

            self.assertFalse((output / "release.json").exists())
            self.assertEqual(list(output.glob(".release-*")), [])

    def test_rejects_missing_windows_zip(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            (source / "openocd-windows-x86_64.zip").unlink()
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output), output)

    def test_rejects_unexpected_seventh_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            (source / "unexpected.txt").write_text("unexpected")
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output), output)

    def test_rejects_unsafe_tag(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output, tag="../1.2.3"), output)

    def test_rejects_output_inside_input(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = source / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output), output)

    def test_rejects_symlink(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            archive = source / "openocd-windows-x86_64.zip"
            replacement = Path(temporary) / "archive.zip"
            archive.rename(replacement)
            archive.symlink_to(replacement)
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output), output)

    def test_rejects_unsupported_type(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(self.run_builder(source, output, type="compiler"), output)

    def test_rejects_safe_but_wrong_repository(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            self.assert_failure_without_manifest(
                self.run_builder(source, output, repository="phlyash/other-openocd"), output
            )

    def test_fast_client_job_runs_both_local_publisher_tests(self):
        """A fast Ubuntu job must execute both local publisher client suites."""
        assert_job_contract(
            self,
            WORKFLOW.read_text(),
            "test-publisher-client",
            EXPECTED_TEST_PUBLISHER_CLIENT_JOB,
        )

    def test_fast_client_contract_rejects_action_text_in_a_later_job(self):
        """A checkout mention outside this job cannot make the fast contract pass."""
        workflow = WORKFLOW.read_text()
        workflow = replace_in_job(
            workflow,
            "test-publisher-client",
            "      - uses: actions/checkout@v4\n",
            "      - run: echo 'uses: actions/checkout@v4'\n",
        )
        workflow += "\n  later-job:\n    runs-on: ubuntu-24.04\n    steps:\n      - uses: actions/checkout@v4\n"
        with self.assertRaises(AssertionError):
            assert_job_contract(
                self, workflow, "test-publisher-client", EXPECTED_TEST_PUBLISHER_CLIENT_JOB
            )

    def test_manual_release_deploy_job_builds_and_transports_same_run_artifacts(self):
        """A manual post-release job must match the complete OpenOCD publisher contract."""
        assert_job_contract(self, WORKFLOW.read_text(), "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_deploy_contract_rejects_direct_release_tag_shell_interpolation(self):
        """A manual tag must enter the shell only through the exact step environment."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "deploy-beget",
            '        env:\n          RELEASE_TAG: ${{ inputs.release_tag }}\n',
            '',
        )
        workflow = replace_in_job(
            workflow,
            "deploy-beget",
            '--tag "$RELEASE_TAG"',
            '--tag "${{ inputs.release_tag }}"',
        )
        with self.assertRaises(AssertionError):
            assert_job_contract(self, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_deploy_contract_rejects_unquoted_release_tag_shell_variable(self):
        """An unquoted tag variable would allow shell word splitting before validation."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "deploy-beget",
            '--tag "$RELEASE_TAG"',
            '--tag $RELEASE_TAG',
        )
        with self.assertRaises(AssertionError):
            assert_job_contract(self, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_safe_workflow_shell_shape_rejects_malicious_tag_without_marker(self):
        """A quote/$() tag is one rejected builder argument and cannot execute shell code."""
        with tempfile.TemporaryDirectory() as temporary:
            source = self.make_input(temporary)
            output = Path(temporary) / "output"
            marker = Path(temporary) / "shell-marker"
            executable_directory = Path(temporary) / "bin"
            executable_directory.mkdir()
            argument_capture = Path(temporary) / "python-arguments"
            python_wrapper = executable_directory / "python3"
            python_wrapper.write_text(
                "#!/usr/bin/env bash\n"
                "printf '%s\\n' \"$@\" >\"$ARGUMENT_CAPTURE\"\n"
                "exec \"$REAL_PYTHON\" \"$@\"\n"
            )
            python_wrapper.chmod(0o700)
            malicious_tag = 'v1.2.3\"; touch \"$MARKER\"; $(touch \"$MARKER\"); #'
            environment = {
                **os.environ,
                "RELEASE_TAG": malicious_tag,
                "MARKER": str(marker),
                "INPUT": str(source),
                "OUTPUT": str(output),
                "ARGUMENT_CAPTURE": str(argument_capture),
                "REAL_PYTHON": sys.executable,
                "PATH": str(executable_directory) + os.pathsep + os.environ["PATH"],
            }
            command = '''python3 .github/scripts/prepare-beget-release.py \\
  --type openocd \\
  --tag "$RELEASE_TAG" \\
  --repository "$GITHUB_REPOSITORY" \\
  --run-id "$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT" \\
  --input "$INPUT" \\
  --output "$OUTPUT"'''
            result = subprocess.run(
                ["bash", "-c", command],
                cwd=ROOT,
                env={
                    **environment,
                    "GITHUB_REPOSITORY": "phlyash/openocd",
                    "GITHUB_RUN_ID": "123456",
                    "GITHUB_RUN_ATTEMPT": "1",
                },
                text=True,
                capture_output=True,
                check=False,
            )
            self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(argument_capture.exists(), result.stdout + result.stderr)
            arguments = argument_capture.read_text().splitlines()
            self.assertEqual(arguments[arguments.index("--tag") + 1], malicious_tag)
            self.assertEqual(arguments.count(malicious_tag), 1)
            self.assertFalse(marker.exists())
            self.assertFalse((output / "release.json").exists())
            self.assertNotIn(malicious_tag, result.stdout + result.stderr)

    def test_deploy_contract_rejects_commented_job_if(self):
        """Commenting manual-only gating must not be accepted as an active job field."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "deploy-beget",
            "    if: github.event_name == 'workflow_dispatch'\n",
            "    # if: github.event_name == 'workflow_dispatch'\n",
        )
        with self.assertRaises(AssertionError):
            assert_job_contract(self, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_deploy_contract_rejects_misindented_job_needs(self):
        """A dependency nested under another field is not a job-level release dependency."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "deploy-beget",
            "    needs: release\n",
            "      needs: release\n",
        )
        with self.assertRaises(AssertionError):
            assert_job_contract(self, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_deploy_contract_rejects_download_action_text_in_shell_or_later_job(self):
        """Only the deploy step itself may supply its required artifact action."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "deploy-beget",
            "        uses: actions/download-artifact@v4\n",
            "        run: echo 'uses: actions/download-artifact@v4'\n",
        )
        workflow += "\n  later-job:\n    runs-on: ubuntu-24.04\n    steps:\n      - uses: actions/download-artifact@v4\n"
        with self.assertRaises(AssertionError):
            assert_job_contract(self, workflow, "deploy-beget", EXPECTED_DEPLOY_BEGET_JOB)

    def test_complete_contract_rejects_missing_pull_request_trigger(self):
        """The fast client job must remain available to pull requests."""
        workflow = WORKFLOW.read_text().replace("  pull_request:\n", "", 1)
        with self.assertRaises(AssertionError):
            assert_complete_workflow_contract(self, workflow)

    def test_complete_workflow_contract(self):
        """The committed workflow must retain the complete publishing contract."""
        assert_complete_workflow_contract(self, WORKFLOW.read_text())

    def test_complete_contract_rejects_platform_dependency_on_fast_client(self):
        """Platform builds cannot be gated behind the independent client signal."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "macos-aarch64",
            "    runs-on: macos-14\n",
            "    needs: test-publisher-client\n    runs-on: macos-14\n",
        )
        with self.assertRaises(AssertionError):
            assert_complete_workflow_contract(self, workflow)

    def test_complete_contract_rejects_multiline_flow_platform_dependency(self):
        """A multiline flow-style dependency also gates the macOS build."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "macos-aarch64",
            "    runs-on: macos-14\n",
            "    needs: [\n      test-publisher-client\n    ]\n    runs-on: macos-14\n",
        )
        with self.assertRaises(AssertionError):
            assert_complete_workflow_contract(self, workflow)

    def test_complete_contract_rejects_quoted_key_platform_dependency(self):
        """A quoted YAML needs key still gates the macOS build."""
        workflow = replace_in_job(
            WORKFLOW.read_text(),
            "macos-aarch64",
            "    runs-on: macos-14\n",
            '    "needs": test-publisher-client\n    runs-on: macos-14\n',
        )
        with self.assertRaises(AssertionError):
            assert_complete_workflow_contract(self, workflow)

    def test_complete_contract_rejects_workflow_contents_write(self):
        """The workflow default must stay read-only for the publisher client."""
        workflow = WORKFLOW.read_text().replace(
            "permissions:\n  contents: read\n",
            "permissions:\n  contents: write\n",
            1,
        )
        with self.assertRaises(AssertionError):
            assert_complete_workflow_contract(self, workflow)


if __name__ == "__main__":
    unittest.main()
