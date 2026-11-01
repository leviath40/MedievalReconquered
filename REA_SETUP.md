Install and configure REA with Ghidra on my computer so this coding agent can perform real native-binary analysis.

Official sources:
- REA: https://github.com/morluto/rea (npm package: rea-agents)
- Ghidra: https://github.com/NationalSecurityAgency/ghidra
- Ghidra releases: https://github.com/NationalSecurityAgency/ghidra/releases

Read the current installation instructions and compatibility requirements from these official projects before choosing versions. Use a published REA release and a Ghidra/JDK combination that it explicitly supports. Before running the npm package, confirm that its repository field points to morluto/rea. The platform notes below reflect REA 6.4.0 and Ghidra 12.1.4; if the current docs differ, follow the docs and tell me what changed.

Complete this in one pass. Ask for my approval once, for the setup plan in step 3. After that, finish everything without further questions unless something fails or needs an action only I can take.

1. Identify my operating system, CPU architecture, shell, and coding client, and whether commands run locally or on a remote host. Keep REA, Ghidra, Java, and their paths in that same environment. On Windows, install and run everything natively on Windows. Don't enable Windows features or virtualization, and don't restart my computer.

2. Check for existing REA, Ghidra, Node.js/npm, and a full Java Development Kit, including javac. Reuse compatible installations. Identify missing prerequisites and any platform limitations.

3. Show me one concise setup plan covering downloads (with sizes), installation locations, required build tools, configuration changes, and anything that needs administrator rights. Prefer user-level installs that avoid admin rights. Wait for my approval, then complete the approved work.

4. Install missing prerequisites and Ghidra from official releases or trusted package managers. Verify published download checksums when available.
   - Check that Ghidra has a working native decompiler for my exact OS and CPU architecture, in Ghidra/Features/Decompiler/os/<platform>/.
   - If the decompiler needs building, follow the matching release's instructions with the build tools for my OS, and include those tools in the plan. Make sure the build targets my CPU architecture, and confirm the result (for example with `file` on macOS or Linux) before installing it.
   - If the operating system's security features block a binary (macOS Gatekeeper, Windows SmartScreen or Defender), stop and tell me instead of changing security settings.

5. Use REA's own setup workflow to register its MCP server and matching reverse-engineer-anything skill. Pick my client's ID from REA's supported-client list, for example claude_code for the Claude Code desktop app or terminal, or codex for Codex desktop or terminal.
   - Set GHIDRA_INSTALL_DIR and JAVA_HOME in the same command that runs setup, so it saves them into the MCP registration. Desktop apps don't inherit shell variables, so the paths must live in the registration.
   - Setup's interactive prompts need a real terminal, so run it with --dry-run first and include its plan in step 3. After my approval, apply it with --yes and an explicit --client.
   - Prefer stable paths that survive package-manager upgrades, such as Homebrew's opt path on macOS rather than a version-numbered Cellar path. Where only a version-numbered path exists (common for Windows JDK installers), note what will need updating after an upgrade.
   - Raise REA_GHIDRA_STARTUP_TIMEOUT_MS (default 330000 ms) in the registration to allow for large binaries.
   - Check the client's own per-call MCP tool timeout, and raise it if it's shorter than Ghidra's analysis might take. Examples: Codex's tool_timeout_sec (default 300 seconds), or Claude Code's per-server timeout.
   - Verify the client's actual configuration locations and overrides. Preserve existing settings, keep setup's backups, and avoid duplicate registrations.

6. Run REA's client, skill, and Ghidra diagnostics. Run the Ghidra check with the same environment variables as the registration; from a plain shell it will wrongly report Ghidra as missing. Distinguish server startup from individual tool-call timeouts.

7. Verify the integration through this coding agent's actual REA MCP tools:
   - Compile a tiny fixture for my platform containing a function with known behaviour. If no C compiler is available, include a lightweight one in the plan (for example Zig or LLVM's clang) rather than a full IDE.
   - Open it with provider_id ghidra, list its functions, and decompile that function.
   - The first Ghidra-backed call waits for the full analysis. Never cancel or retry it partway through: cancelling shuts Ghidra down, and the next call starts over.
   - Show the expected behaviour next to the actual decompiled result, and preserve the evidence.
   - Close the analysis session, and confirm that no Ghidra process is left running.

Platform notes:
- macOS (Intel or Apple Silicon): Official Ghidra archives may not include a macOS decompiler. If so, build it from the source included in the release, using the Xcode Command Line Tools and make, then install it to os/mac_arm_64 or os/mac_x86_64. On Apple Silicon, the default macOS build may produce an x86_64 binary; it must be arm64.
- Linux x64: The official release includes the linux_x86_64 decompiler.
- Linux ARM64: The linux_arm_64 decompiler may need building (gcc/g++, make, and bison/flex if required).
