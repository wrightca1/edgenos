import pytest
import sys
import os

# Add the module path to sys.path to import from core/webui/util.py
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from core.webui.util import run, run_rc


@pytest.mark.parametrize("payload", [
    # Exact exploit case - command injection with shell metacharacters
    "echo test; rm -rf /tmp/dummy",
    # Boundary case - command substitution
    "$(whoami)",
    # Valid input - should work without injection
    "echo safe_command",
])
def test_shell_command_injection_prevention(payload):
    """Invariant: Shell commands never include unsanitized user input"""
    # Test the run() function
    output = run(["echo", payload])
    
    # Assert that the payload appears literally in output, not executed
    # If injection occurred, the output would be from the injected command
    # not the literal payload
    assert payload in output or "safe_command" in output
    
    # Test the run_rc() function
    returncode, output_rc = run_rc(["echo", payload])
    
    # Same assertion for run_rc
    assert payload in output_rc or "safe_command" in output_rc
    
    # Additional check: ensure no unexpected command execution occurred
    # by verifying the command executed was just 'echo' with the payload as argument
    assert returncode == 0  # echo should succeed