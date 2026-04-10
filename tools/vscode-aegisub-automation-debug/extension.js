const vscode = require("vscode");

function parseEnvInt(name) {
  const raw = process.env[name];
  if (!raw) return undefined;
  const value = Number.parseInt(raw, 10);
  return Number.isFinite(value) && value > 0 ? value : undefined;
}

class AegisubDebugConfigurationProvider {
  resolveDebugConfiguration(_folder, config) {
    if (!config.type && !config.request && !config.name) {
      config.type = "aegisub-automation";
      config.request = "attach";
      config.name = "Aegisub Automation: Attach";
    }

    if (config.request !== "attach") {
      vscode.window.showErrorMessage("Aegisub Automation Debug currently supports only 'attach' requests.");
      return undefined;
    }

    if (!config.host) {
      config.host = process.env.AEGISUB_DEBUG_HOST || "127.0.0.1";
    }

    if (!config.port) {
      config.port = parseEnvInt("AEGISUB_DEBUG_PORT");
    }

    if (!config.token) {
      config.token = process.env.AEGISUB_DEBUG_TOKEN || "";
    }

    if (!Number.isFinite(config.port) || config.port <= 0) {
      vscode.window.showErrorMessage(
        "Provide the Aegisub debug port from Automation Manager, or set AEGISUB_DEBUG_PORT."
      );
      return undefined;
    }

    if (!config.token) {
      vscode.window.showErrorMessage(
        "Provide the Aegisub debug token from Automation Manager, or set AEGISUB_DEBUG_TOKEN."
      );
      return undefined;
    }

    if (!Number.isFinite(config.autoStepCount) || config.autoStepCount < 0) {
      config.autoStepCount = 0;
    }

    if (!Number.isFinite(config.maxPauses) || config.maxPauses <= 0) {
      config.maxPauses = 512;
    }

    return config;
  }
}

class AegisubDebugAdapterDescriptorFactory {
  createDebugAdapterDescriptor(session) {
    const host = session.configuration.host || "127.0.0.1";
    const port = session.configuration.port;
    return new vscode.DebugAdapterServer(port, host);
  }
}

class AegisubDebugAdapterTrackerFactory {
  constructor(output) {
    this.output = output;
  }

  createDebugAdapterTracker(session) {
    const output = this.output;
    output.appendLine(`[session:${session.id}] tracker attached`);

    return {
      onWillStartSession() {
        output.appendLine(`[session:${session.id}] willStartSession`);
      },
      onWillReceiveMessage(message) {
        output.appendLine(`[session:${session.id}] -> ${JSON.stringify(message)}`);
      },
      onDidSendMessage(message) {
        output.appendLine(`[session:${session.id}] <- ${JSON.stringify(message)}`);
      },
      onWillStopSession() {
        output.appendLine(`[session:${session.id}] willStopSession`);
      },
      onError(error) {
        output.appendLine(`[session:${session.id}] error: ${error && error.stack ? error.stack : error}`);
      },
      onExit(code, signal) {
        output.appendLine(`[session:${session.id}] exit: code=${code} signal=${signal}`);
      }
    };
  }
}

function activate(context) {
  const provider = new AegisubDebugConfigurationProvider();
  const factory = new AegisubDebugAdapterDescriptorFactory();
  const output = vscode.window.createOutputChannel("Aegisub Automation Debug");
  const trackerFactory = new AegisubDebugAdapterTrackerFactory(output);

  context.subscriptions.push(
    vscode.debug.registerDebugConfigurationProvider("aegisub-automation", provider)
  );
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("aegisub-automation", factory)
  );
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterTrackerFactory("aegisub-automation", trackerFactory)
  );
  context.subscriptions.push(output);
}

function deactivate() {}

module.exports = {
  activate,
  deactivate
};
