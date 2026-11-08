'use strict';

const vscode = require('vscode');
const path = require('path');

const isWindows = process.platform === 'win32';

function getConfig() {
	return vscode.workspace.getConfiguration('vortex');
}

// Quote a single shell argument. Double quotes work for both cmd/PowerShell and
// POSIX shells for the paths we pass here.
function quoteArg(value) {
	return isWindows
		? '"' + String(value).replace(/"/g, '""') + '"'
		: "'" + String(value).replace(/'/g, "'\\''") + "'";
}

// Build a command line. The executable itself must stay unquoted on Windows:
// PowerShell treats a quoted bare word as a string instead of a command.
function buildCommand(exe, args) {
	let head = exe;
	if (/\s/.test(exe)) {
		head = isWindows ? '& ' + quoteArg(exe) : quoteArg(exe);
	}
	return [head].concat(args.map(quoteArg)).join(' ');
}

function activeVortexDocument() {
	const editor = vscode.window.activeTextEditor;
	if (!editor || editor.document.languageId !== 'vortex') {
		vscode.window.showWarningMessage('请先打开一个 .vt 文件。');
		return null;
	}
	return editor.document;
}

async function saveIfDirty(document) {
	if (document.isDirty) {
		await document.save();
	}
}

function terminalFor(document, name) {
	const term = vscode.window.createTerminal({
		name: name,
		cwd: path.dirname(document.fileName)
	});
	term.show();
	return term;
}

async function runFile() {
	const document = activeVortexDocument();
	if (!document) {
		return;
	}
	await saveIfDirty(document);

	const config = getConfig();
	const interpreter = config.get('interpreterPath') || 'vortex';
	const extraArgs = config.get('interpreterArgs') || [];
	const cmd = buildCommand(interpreter, extraArgs.concat([document.fileName]));

	terminalFor(document, 'Vortex: Run').sendText(cmd);
}

function outputPathFor(document) {
	const dir = path.dirname(document.fileName);
	const base = path.basename(document.fileName).replace(/\.vt$/i, '');
	return path.join(dir, isWindows ? base + '.exe' : base);
}

async function compileFile() {
	const document = activeVortexDocument();
	if (!document) {
		return;
	}
	await saveIfDirty(document);

	const config = getConfig();
	const compiler = config.get('compilerPath') || 'vtcc';
	const optimization = config.get('optimization') || '';
	const output = outputPathFor(document);

	const args = ['build', document.fileName, '-o', output];
	if (optimization) {
		args.push(optimization);
	}

	const term = terminalFor(document, 'Vortex: Compile');
	term.sendText(buildCommand(compiler, args));

	if (config.get('runAfterCompile')) {
		term.sendText(buildCommand(output, []));
	}
}

function activate(context) {
	context.subscriptions.push(
		vscode.commands.registerCommand('vortex.runFile', runFile),
		vscode.commands.registerCommand('vortex.compileFile', compileFile)
	);
}

function deactivate() {}

module.exports = { activate, deactivate };
