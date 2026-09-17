// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ChatMessage, ConversationState, InstalledLocalModel, LocalModelState, PageHandler} from './ark.mojom-webui.js';

function get<T extends HTMLElement>(id: string): T {
  const element = document.getElementById(id);
  if (!element) {
    throw new Error(`Missing Ark element: ${id}`);
  }
  return element as T;
}

const routes = ['home', 'chat', 'models', 'advanced'] as const;
type Route = typeof routes[number];
const draft = get<HTMLTextAreaElement>('draft');
const clearButton = get<HTMLButtonElement>('clear-draft');
const dialog = get<HTMLDialogElement>('clear-dialog');
const GEMINI_KEY_STORAGE = 'ark_gemini_api_key';
const SELECTED_MODEL_STORAGE = 'ark_selected_model';
const RAIL_COLLAPSED_STORAGE = 'ark_rail_collapsed';
const RAIL_WIDTH_STORAGE = 'ark_rail_width';
const pageHandler = PageHandler.getRemote();
let currentRoute: Route = 'home';
let newChatPending = false;
let freshChat = false;
let conversationId = '';
let conversationStateEpoch = 0;
let conversationsList: ConversationState[] = [];
let currentMessages: ChatMessage[] = [];
let draftSaveTimer = 0;
let aiSearchMode = false;
const PREPARED_LOCAL_MODEL = 'local:mlx:llama-3.2-11b-vision-instruct';
let activeModel = PREPARED_LOCAL_MODEL;
const generatingConversationIds = new Set<string>();
const pendingUserMessages = new Map<string, ChatMessage>();
let currentLocalModelState: LocalModelState | null = null;
let installedLocalModels: InstalledLocalModel[] = [];
const isSidePanel = location.host === 'ark-side-panel';

function isSelectableLocalModel(model: InstalledLocalModel): boolean {
  return model.runtimeCompatible || model.runtimeBackend === 'llama.cpp';
}

if (isSidePanel) {
  document.documentElement.classList.add('side-panel-surface');
}

function setRailCollapsed(collapsed: boolean): void {
  if (isSidePanel) {
    return;
  }
  document.documentElement.classList.toggle('rail-collapsed', collapsed);
  const toggle = get<HTMLButtonElement>('rail-toggle');
  toggle.ariaExpanded = String(!collapsed);
  toggle.setAttribute('aria-label', collapsed ? 'Expand navigation' : 'Collapse navigation');
  toggle.title = collapsed ? 'Expand navigation' : 'Collapse navigation';
  localStorage.setItem(RAIL_COLLAPSED_STORAGE, String(collapsed));
}

function setRailWidth(width: number): void {
  const maxWidth = Math.max(260, Math.min(360, window.innerWidth - 120));
  const nextWidth = Math.round(Math.max(180, Math.min(maxWidth, width)));
  document.documentElement.style.setProperty('--rail-expanded-width', `${nextWidth}px`);
  localStorage.setItem(RAIL_WIDTH_STORAGE, String(nextWidth));
  get('rail-resize').setAttribute('aria-valuenow', String(nextWidth));
}

get('rail-toggle').addEventListener('click', () => {
  setRailCollapsed(!document.documentElement.classList.contains('rail-collapsed'));
});
setRailCollapsed(localStorage.getItem(RAIL_COLLAPSED_STORAGE) === 'true');
const storedRailWidth = Number(localStorage.getItem(RAIL_WIDTH_STORAGE));
setRailWidth(Number.isFinite(storedRailWidth) && storedRailWidth > 0 ? storedRailWidth : 220);

const railResize = get('rail-resize');
let resizeStartX = 0;
let resizeStartWidth = 220;
railResize.addEventListener('pointerdown', (event: PointerEvent) => {
  if (document.documentElement.classList.contains('rail-collapsed')) return;
  resizeStartX = event.clientX;
  resizeStartWidth = parseFloat(getComputedStyle(document.documentElement)
                                    .getPropertyValue('--rail-expanded-width')) || 220;
  railResize.setPointerCapture(event.pointerId);
  document.documentElement.classList.add('rail-resizing');
});
railResize.addEventListener('pointermove', (event: PointerEvent) => {
  if (!railResize.hasPointerCapture(event.pointerId)) return;
  setRailWidth(resizeStartWidth + event.clientX - resizeStartX);
});
railResize.addEventListener('pointerup', (event: PointerEvent) => {
  if (!railResize.hasPointerCapture(event.pointerId)) return;
  railResize.releasePointerCapture(event.pointerId);
  document.documentElement.classList.remove('rail-resizing');
});

function showArkNotice(title: string, message: string): void {
  get('ark-notice-title').textContent = title;
  get('ark-notice-message').textContent = message;
  get<HTMLDialogElement>('ark-notice-dialog').showModal();
}

function confirmModelDelete(displayName: string): Promise<boolean> {
  const modelDialog = get<HTMLDialogElement>('model-delete-dialog');
  get('model-delete-message').textContent =
      `Delete ${displayName} from this Mac? Downloaded weights will be removed; chats remain intact.`;
  modelDialog.showModal();
  return new Promise(resolve => {
    modelDialog.addEventListener('close', () => resolve(modelDialog.returnValue === 'delete'),
                                 {once: true});
  });
}


function autoResizeTextarea(): void {
  draft.style.height = 'auto';
  const maxH = 200;
  draft.style.height = Math.min(draft.scrollHeight, maxH) + 'px';
  draft.style.overflowY = draft.scrollHeight > maxH ? 'auto' : 'hidden';
}

function saveDraft(): void {
  autoResizeTextarea();
  clearButton.disabled = !draft.value;
  updateSendButtonState();
  window.clearTimeout(draftSaveTimer);
  if (!conversationId) {
    return;
  }
  draftSaveTimer = window.setTimeout(async () => {
    try {
      const {success} = await pageHandler.saveDraft(
          conversationId, draft.value);
      if (!success) {
        throw new Error('Draft was not stored');
      }
    } catch {
      get('draft-status').textContent = 'Ark could not save this draft locally.';
    }
  }, 180);
}

draft.addEventListener('input', saveDraft);
draft.addEventListener('input', autoResizeTextarea);
autoResizeTextarea();

draft.addEventListener('keydown', (e: KeyboardEvent) => {
  if (e.key === 'Enter' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
    e.preventDefault();
    const sendBtn = document.querySelector<HTMLButtonElement>('.send-button');
    if (sendBtn && !sendBtn.disabled) {
      sendBtn.click();
    }
  }
});

function updateSendButtonState(): void {
  const sendBtn = document.querySelector<HTMLButtonElement>('.send-button');
  if (!sendBtn) {
    return;
  }
  const hasText = Boolean(draft.value.trim());
  const isInstalled = installedLocalModels.some(
      model => model.modelId === activeModel && isSelectableLocalModel(model));
  const hasGemini = Boolean(localStorage.getItem(GEMINI_KEY_STORAGE)?.trim());
  const isReady = activeModel.startsWith('local:') ? isInstalled : hasGemini;

  const isGenerating = Boolean(
      conversationId && generatingConversationIds.has(conversationId));
  sendBtn.disabled = !hasText || !isReady || isGenerating;
  if (isGenerating) {
    sendBtn.title = 'Generating response…';
  } else if (!isReady) {
    sendBtn.title = activeModel.startsWith('local:') ?
        'The selected local model is not downloaded yet. Go to Models to download.' :
        'Gemini API key is required. Go to Models > Cloud providers to configure.';
  } else if (!hasText) {
    sendBtn.title = 'Type a message to send';
  } else {
    sendBtn.title = 'Send message (Enter)';
  }
}

function determineSelectedModel(hasGemini: boolean): string {
  const lastSelected = localStorage.getItem(SELECTED_MODEL_STORAGE);
  const validModels = [
    ...installedLocalModels.filter(isSelectableLocalModel)
        .map(model => model.modelId),
    'cloud:gemini-2.5-flash',
    'cloud:gemini-2.5-pro',
  ];
  if (lastSelected && validModels.includes(lastSelected)) {
    return lastSelected;
  }
  const firstLocal = installedLocalModels.find(isSelectableLocalModel);
  if (firstLocal) {
    return firstLocal.modelId;
  }
  if (hasGemini) {
    return 'cloud:gemini-2.5-flash';
  }
  return PREPARED_LOCAL_MODEL;
}

function updateModelStatusUI(modelId: string): void {
  activeModel = modelId;
  const installed = installedLocalModels.find(model => model.modelId === modelId);
  const isInstalled = Boolean(installed && isSelectableLocalModel(installed));
  const geminiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  const hasGemini = Boolean(geminiKey);

  const homeDot = get('home-status-dot');
  const composerDot = get('composer-status-dot');
  const homeCaption = get('home-ai-caption');
  const composerCaptionText = get('composer-caption-text');

  homeDot.className = 'status-dot';
  composerDot.className = 'status-dot';

  if (modelId.startsWith('local:')) {
    if (isInstalled) {
      homeDot.classList.add('active');
      composerDot.classList.add('active');
      homeCaption.textContent = 'On-device Metal · Ready';
      composerCaptionText.textContent =
          `Connected to ${installed?.displayName || 'local model'} · On-device Metal`;
    } else {
      homeDot.classList.add('prepared');
      composerDot.classList.add('prepared');
      homeCaption.textContent = 'On-device Metal · Needs Download';
      composerCaptionText.textContent =
          'No compatible local model selected · Go to Models to download';
    }
  } else if (modelId.startsWith('cloud:gemini')) {
    if (hasGemini) {
      homeDot.classList.add('active');
      composerDot.classList.add('active');
      homeCaption.textContent = 'Google Cloud · Ready';
      composerCaptionText.textContent =
          `Connected to ${modelId === 'cloud:gemini-2.5-pro' ? 'Gemini 2.5 Pro' : 'Gemini 2.5 Flash'} · Google Cloud`;
    } else {
      homeDot.classList.add('prepared');
      composerDot.classList.add('prepared');
      homeCaption.textContent = 'API key required';
      composerCaptionText.textContent =
          'Gemini API key required · Configure in Cloud Providers';
    }
  }

  updateSendButtonState();
}

function getModelDisplayName(modelId: string): string {
  const installed = installedLocalModels.find(model => model.modelId === modelId);
  if (installed) {
    return `${installed.displayName} (Local)`;
  }
  switch (modelId) {
    case PREPARED_LOCAL_MODEL:
      return 'Llama 3.2 11B Vision Instruct (Local · MLX)';
    case 'cloud:gemini-2.5-flash':
      return 'Gemini 2.5 Flash (Cloud)';
    case 'cloud:gemini-2.5-pro':
      return 'Gemini 2.5 Pro (Cloud)';
    default:
      return 'Choose a model';
  }
}

interface ModelMenu {
  root: HTMLElement;
  trigger: HTMLButtonElement;
  value: HTMLElement;
  listbox: HTMLElement;
}

const modelMenus: ModelMenu[] = [
  {
    root: get('composer-model-picker'),
    trigger: get<HTMLButtonElement>('composer-model-trigger'),
    value: get('composer-model-value'),
    listbox: get('composer-model-menu'),
  },
  {
    root: get('home-model-picker'),
    trigger: get<HTMLButtonElement>('home-model-trigger'),
    value: get('home-model-value'),
    listbox: get('home-model-menu'),
  },
];

interface ModelMenuEntry {
  value: string;
  name: string;
  meta: string;
  disabled?: boolean;
}

function getModelMenuEntries(hasGemini: boolean): Array<{label: string; entries: ModelMenuEntry[]}> {
  const localEntries: ModelMenuEntry[] = installedLocalModels.length === 0 ? [{
    value: PREPARED_LOCAL_MODEL,
    name: 'No local model installed',
    meta: 'Open Models to download',
  }] : installedLocalModels.map(model => ({
    value: model.modelId,
    name: model.displayName,
    meta: `${model.variant} · ${model.runtimeBackend}${model.runtimeCompatible ? '' : ' · Incompatible'}`,
    disabled: !isSelectableLocalModel(model),
  }));
  return [
    {label: 'Local Models · Apple Silicon Metal', entries: localEntries},
    {label: 'Cloud Models · Google AI Studio', entries: [
      {
        value: 'cloud:gemini-2.5-flash',
        name: 'Gemini 2.5 Flash',
        meta: hasGemini ? 'Cloud · Ready' : 'Cloud · API key required',
      },
      {
        value: 'cloud:gemini-2.5-pro',
        name: 'Gemini 2.5 Pro',
        meta: hasGemini ? 'Cloud · Ready' : 'Cloud · API key required',
      },
    ]},
    {label: 'Settings', entries: [{
      value: 'action:manage',
      name: 'Manage models',
      meta: 'Open model settings',
    }]},
  ];
}

function closeModelMenus(): void {
  for (const menu of modelMenus) {
    menu.listbox.hidden = true;
    menu.trigger.ariaExpanded = 'false';
  }
}

function setModelMenuValues(value: string): void {
  const hasGemini = Boolean((localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim());
  const entries = getModelMenuEntries(hasGemini).flatMap(group => group.entries);
  const selected = entries.find(entry => entry.value === value);
  for (const menu of modelMenus) {
    menu.value.textContent = selected ? `${selected.name} · ${selected.meta}` : 'Choose a model';
    for (const option of menu.listbox.querySelectorAll<HTMLElement>('[role="option"]')) {
      const isSelected = option.dataset['modelValue'] === value;
      option.ariaSelected = String(isSelected);
      option.querySelector<HTMLElement>('.model-menu-check')!.textContent = isSelected ? '✓' : '';
    }
  }
}

function chooseModel(value: string): void {
  closeModelMenus();
  if (value === 'action:manage') {
    location.hash = 'models';
    return;
  }
  localStorage.setItem(SELECTED_MODEL_STORAGE, value);
  setModelMenuValues(value);
  updateModelStatusUI(value);
  if (conversationId) {
    void pageHandler.updateConversationModel(conversationId, value);
    const conv = conversationsList.find(c => c.id === conversationId);
    if (conv) {
      conv.modelName = value;
    }
  }
}

function renderModelMenus(selected: string): void {
  const geminiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  const hasGemini = Boolean(geminiKey);
  const groups = getModelMenuEntries(hasGemini);
  activeModel = selected;
  for (const menu of modelMenus) {
    menu.listbox.replaceChildren();
    for (const group of groups) {
      const heading = document.createElement('div');
      heading.className = 'model-menu-group-label';
      heading.textContent = group.label;
      menu.listbox.appendChild(heading);
      for (const entry of group.entries) {
        const option = document.createElement('button');
        option.type = 'button';
        option.className = 'model-menu-option';
        option.setAttribute('role', 'option');
        option.dataset['modelValue'] = entry.value;
        option.ariaSelected = String(entry.value === selected);
        option.disabled = Boolean(entry.disabled);
        const check = document.createElement('span');
        check.className = 'model-menu-check';
        check.setAttribute('aria-hidden', 'true');
        check.textContent = entry.value === selected ? '✓' : '';
        const copy = document.createElement('span');
        copy.className = 'model-menu-option-copy';
        const name = document.createElement('span');
        name.className = 'model-menu-option-name';
        name.textContent = entry.name;
        const meta = document.createElement('span');
        meta.className = 'model-menu-option-meta';
        meta.textContent = entry.meta;
        copy.append(name, meta);
        option.append(check, copy);
        option.addEventListener('click', () => chooseModel(entry.value));
        menu.listbox.appendChild(option);
      }
    }
  }
  setModelMenuValues(selected);
  updateModelStatusUI(selected);
}

function refreshModelMenus(): void {
  renderModelMenus(determineSelectedModel(
      Boolean(localStorage.getItem(GEMINI_KEY_STORAGE)?.trim())));
}

function openModelMenu(menu: ModelMenu): void {
  const isOpen = !menu.listbox.hidden;
  closeModelMenus();
  if (isOpen) {
    return;
  }
  menu.listbox.hidden = false;
  menu.trigger.ariaExpanded = 'true';
  const selected = menu.listbox.querySelector<HTMLElement>('[aria-selected="true"]');
  selected?.focus();
}

for (const menu of modelMenus) {
  menu.trigger.addEventListener('click', () => openModelMenu(menu));
  menu.trigger.addEventListener('keydown', event => {
    if (event.key === 'Enter' || event.key === ' ' || event.key === 'ArrowDown') {
      event.preventDefault();
      openModelMenu(menu);
    }
  });
  menu.listbox.addEventListener('keydown', event => {
    const options = [...menu.listbox.querySelectorAll<HTMLButtonElement>('[role="option"]:not(:disabled)')];
    const current = options.indexOf(document.activeElement as HTMLButtonElement);
    if (event.key === 'Escape') {
      event.preventDefault();
      closeModelMenus();
      menu.trigger.focus();
      return;
    }
    if (event.key === 'ArrowDown' || event.key === 'ArrowUp') {
      event.preventDefault();
      const next = event.key === 'ArrowDown' ? (current + 1) % options.length :
                                                  (current - 1 + options.length) % options.length;
      options[next]?.focus();
    }
    if ((event.key === 'Enter' || event.key === ' ') && document.activeElement instanceof HTMLButtonElement) {
      event.preventDefault();
      document.activeElement.click();
    }
  });
}
document.addEventListener('pointerdown', event => {
  if (!modelMenus.some(menu => menu.root.contains(event.target as Node))) {
    closeModelMenus();
  }
});
refreshModelMenus();

async function refreshInstalledModels(): Promise<void> {
  try {
    const {models} = await pageHandler.getInstalledLocalModels();
    installedLocalModels = models;
    refreshModelMenus();
    renderInstalledModels();
  } catch (error) {
    console.warn('Failed to enumerate installed local models:', error);
  }
}

function renderInstalledModels(): void {
  const container = get('installed-models');
  const count = get('installed-model-count');
  container.replaceChildren();
  count.textContent = `${installedLocalModels.length} INSTALLED`;
  if (installedLocalModels.length === 0) {
    const empty = document.createElement('div');
    empty.className = 'installed-empty';
    empty.textContent = 'No local models installed yet. Search Hugging Face or use the prepared model below.';
    container.appendChild(empty);
    return;
  }
  for (const model of installedLocalModels) {
    const card = document.createElement('article');
    card.className = 'installed-model-card';
    const copy = document.createElement('div');
    const heading = document.createElement('h3');
    heading.textContent = model.displayName;
    const meta = document.createElement('p');
    meta.textContent = `${model.runtimeBackend} · ${model.variant} · ${formatBytes(model.bytesTotal)} · ${model.repository}`;
    copy.append(heading, meta);
    const actions = document.createElement('div');
    actions.className = 'installed-model-actions';
    const useButton = document.createElement('button');
    useButton.type = 'button';
    useButton.className = 'primary';
    useButton.textContent = activeModel === model.modelId ? 'Selected' : 'Use in chat';
    useButton.disabled = activeModel === model.modelId || !isSelectableLocalModel(model);
    useButton.addEventListener('click', () => {
      localStorage.setItem(SELECTED_MODEL_STORAGE, model.modelId);
      updateModelStatusUI(model.modelId);
      refreshModelMenus();
      if (conversationId) void pageHandler.updateConversationModel(conversationId, model.modelId);
      renderInstalledModels();
    });
    const deleteButton = document.createElement('button');
    deleteButton.type = 'button';
    deleteButton.className = 'button-danger';
    deleteButton.textContent = 'Delete';
    deleteButton.addEventListener('click', async () => {
      if (!await confirmModelDelete(model.displayName)) return;
      deleteButton.disabled = true;
      deleteButton.textContent = 'Deleting…';
      const {state} = await pageHandler.deleteLocalModel(model.modelId);
      updateLocalModelUI(state);
      await refreshInstalledModels();
    });
    actions.append(useButton, deleteButton);
    card.append(copy, actions);
    container.appendChild(card);
  }
}

function renderTypingIndicator(container: HTMLElement): void {
  const dots = document.createElement('span');
  dots.className = 'typing-dots';
  for (let i = 0; i < 3; i++) {
    dots.appendChild(document.createElement('span'));
  }
  container.replaceChildren(dots);
}

function highlightCode(code: string, lang: string): string {
  let esc = code.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  const aliases: Record<string, string> = {
    'c#': 'csharp', 'cs': 'csharp', 'dotnet': 'csharp',
    'c++': 'cpp', 'cc': 'cpp', 'cxx': 'cpp',
    'ts': 'typescript', 'js': 'javascript', 'jsx': 'javascript',
    'py': 'python', 'rb': 'ruby', 'ps': 'powershell', 'ps1': 'powershell',
    'shell': 'bash', 'sh': 'bash', 'zsh': 'bash',
    'yml': 'yaml', 'htm': 'html', 'xml': 'html', 'svg': 'html',
    'postgres': 'sql', 'postgresql': 'sql', 'mysql': 'sql', 'sqlite': 'sql',
    'golang': 'go', 'kt': 'kotlin', 'rs': 'rust', 'csx': 'csharp',
  };
  lang = aliases[lang.toLowerCase()] || lang.toLowerCase();

  const jsKeywords = 'const let var function return if else for while class import export from async await new this typeof instanceof try catch throw switch case break continue default void null undefined true false';
  const pyKeywords = 'def class import from return if elif else for while try except with as lambda yield pass break continue and or not in is None True False self';
  const shKeywords = 'if then else fi for do done while case esac function echo export source';
  const cKeywords = 'int char float double void return if else for while do switch case break continue struct typedef';
  const rsKeywords = 'fn let mut pub struct enum impl trait use mod match if else for while loop return self Self async await where type const static ref move unsafe extern crate super true false';
  const csharpKeywords = 'abstract as base bool break byte case catch char checked class const continue decimal default delegate do double else enum event explicit extern false finally fixed float for foreach goto if implicit in int interface internal is lock long namespace new null object operator out override params private protected public readonly ref sbyte sealed short sizeof stackalloc static string struct switch this throw true try typeof uint ulong unchecked unsafe ushort using virtual void volatile while async await get set init record required global var dynamic nameof';
  const javaKeywords = 'abstract assert boolean break byte case catch char class const continue default do double else enum extends final finally float for goto if implements import instanceof int interface long native new null package private protected public return short static strictfp super switch synchronized this throw throws transient try void volatile while true false record sealed permits non-sealed var';
  const sqlKeywords = 'select from where join inner left right full outer on as and or not null true false insert into values update set delete create alter drop table database schema index view primary key foreign references unique check constraint group by having order asc desc limit offset union all distinct case when then else end exists between like in is grant revoke commit rollback begin transaction with recursive returning';
  const goKeywords = 'break default func interface select case defer go map struct chan else goto package switch const fallthrough if range type continue for import return var nil true false';
  const kotlinKeywords = 'as break class continue do else false for fun if in interface is null object package return super this throw true try typealias val var when while by catch constructor delegate dynamic field file finally get import init param property receiver set setparam where actual abstract annotation companion const crossinline data enum expect external final infix inline inner internal lateinit noinline open operator out override private protected public reified sealed suspend tailrec vararg';
  const swiftKeywords = 'associatedtype class deinit enum extension fileprivate func import init inout internal let open operator private protocol public rethrows static struct subscript typealias var break continue default defer do else fallthrough for guard if in repeat return switch where while as catch false is nil super self Self throw throws true try actor any async await some nonisolated';
  const rubyKeywords = 'BEGIN END alias and begin break case class def defined do else elsif end ensure false for if in module next nil not or redo rescue retry return self super then true undef unless until when while yield require include attr_reader attr_writer';
  const phpKeywords = 'abstract and array as break callable case catch class clone const continue declare default do echo else elseif empty enddeclare endfor endforeach endif endswitch endwhile eval exit extends final finally for foreach function global goto if implements include include_once instanceof insteadof interface isset list namespace new or print private protected public require require_once return static switch throw trait try unset use var while xor yield true false null';
  const scalaKeywords = 'abstract case catch class def do else extends false final finally for forSome if implicit import lazy match new null object override package private protected return sealed super this throw trait true try type val var while with yield given enum export extension inline opaque open transparent';
  const dartKeywords = 'abstract as assert async await break case catch class const continue covariant default deferred do dynamic else enum export extends extension external factory false final finally for get hide if implements import in interface is late library mixin new null on operator part required rethrow return set show static super switch sync this throw true try typedef var void while with yield';
  const luaKeywords = 'and break do else elseif end false for function goto if in local nil not or repeat return then true until while';
  const powershellKeywords = 'begin break catch class continue data do dynamicparam else elseif end exit filter finally for foreach from function if in param process return switch throw trap try until using var while workflow parallel sequence public private static hidden';
  const yamlKeywords = 'true false null yes no on off include anchors aliases';

  let keywords = '';
  if (lang === 'javascript' || lang === 'typescript') keywords = jsKeywords;
  else if (lang === 'python') keywords = pyKeywords;
  else if (lang === 'bash') keywords = shKeywords;
  else if (lang === 'c' || lang === 'cpp') keywords = cKeywords;
  else if (lang === 'rust') keywords = rsKeywords;
  else if (lang === 'csharp') keywords = csharpKeywords;
  else if (lang === 'java') keywords = javaKeywords;
  else if (lang === 'sql') keywords = sqlKeywords;
  else if (lang === 'go') keywords = goKeywords;
  else if (lang === 'kotlin') keywords = kotlinKeywords;
  else if (lang === 'swift') keywords = swiftKeywords;
  else if (lang === 'ruby') keywords = rubyKeywords;
  else if (lang === 'php') keywords = phpKeywords;
  else if (lang === 'scala') keywords = scalaKeywords;
  else if (lang === 'dart') keywords = dartKeywords;
  else if (lang === 'lua') keywords = luaKeywords;
  else if (lang === 'powershell') keywords = powershellKeywords;
  else if (lang === 'yaml' || lang === 'toml') keywords = yamlKeywords;

  if (keywords) {
    const kws = keywords.split(' ').join('|');
    const lineComment = lang === 'sql' ? '(?:--|#).*' :
        (lang === 'python' || lang === 'bash' || lang === 'ruby' ||
         lang === 'powershell' || lang === 'yaml' || lang === 'toml') ? '#.*' :
        lang === 'lua' ? '--.*' : '//.*';
    const tokenRegex = new RegExp(`(${lineComment}|/\\*[\\s\\S]*?\\*/)|(["'\`][\\s\\S]*?["'\`])|(\\b\\d+\\.?\\d*\\b)|(\\b(?:${kws})\\b)|(\\b\\w+)(?=\\s*\\()`, 'gi');

    esc = esc.replace(tokenRegex, (match, comment, str, num, kw, func) => {
      if (comment) return `<span class="tok-comment">${comment}</span>`;
      if (str) return `<span class="tok-string">${str}</span>`;
      if (num) return `<span class="tok-number">${num}</span>`;
      if (kw) return `<span class="tok-keyword">${kw}</span>`;
      if (func) return `<span class="tok-function">${func}</span>`;
      return match;
    });
  } else if (lang === 'html') {
    esc = esc.replace(/(&lt;\/?[\w:-]+)(.*?)(&gt;)/g, (_match, p1, p2, p3) => {
      const attrs = p2.replace(/([\w-]+)=(&quot;.*?&quot;|&#39;.*?&#39;)/g, '<span class="tok-attr">$1</span>=<span class="tok-string">$2</span>');
      return `<span class="tok-tag">${p1}</span>${attrs}<span class="tok-tag">${p3}</span>`;
    });
  } else if (lang === 'json' || lang === 'jsonc') {
    esc = esc.replace(/(&quot;.*?&quot;)\s*:/g, '<span class="tok-keyword">$1</span>:');
    esc = esc.replace(/: \s*(&quot;.*?&quot;)/g, ': <span class="tok-string">$1</span>');
    esc = esc.replace(/\b(\d+)\b/g, '<span class="tok-number">$1</span>');
    esc = esc.replace(/\b(true|false|null)\b/g, '<span class="tok-builtin">$1</span>');
  }

  return esc;
}

function attachCopyHandlers(container: HTMLElement): void {
  container.querySelectorAll<HTMLButtonElement>('.md-copy-btn').forEach(btn => {
    btn.addEventListener('click', () => {
      const code = btn.dataset['code'] || '';
      navigator.clipboard.writeText(code).then(() => {
        btn.textContent = 'Copied!';
        setTimeout(() => { btn.textContent = 'Copy'; }, 2000);
      }).catch(() => {
        btn.textContent = 'Failed';
        setTimeout(() => { btn.textContent = 'Copy'; }, 2000);
      });
    });
  });
}

function inferCodeLanguage(code: string): string {
  if (/\b(using\s+System|namespace\s+[A-Za-z_]|Console\.(Write|Read)|\b(public|private|internal)\s+(class|interface|record))\b/.test(code)) {
    return 'csharp';
  }
  if (/\b(def|elif|print|input)\b|:\s*$/m.test(code) &&
      /\b(def|print|input|import)\b/.test(code)) {
    return 'python';
  }
  if (/\b(const|let|var|function|console\.log)\b/.test(code)) {
    return 'javascript';
  }
  if (/\b(SELECT|INSERT|UPDATE|DELETE)\b/i.test(code)) {
    return 'sql';
  }
  return 'code';
}

function renderMarkdown(text: string): string {
  if (!text) {
    return '';
  }

  // 1. Normalize line endings
  let src = text.replace(/\r\n/g, '\n');

  // 2. Extract code blocks first so inner content is preserved untouched
  const codeBlocks: {lang: string, code: string}[] = [];
  src = src.replace(/```([a-zA-Z0-9_+.#-]*)[ \t]*\n([\s\S]*?)```/g, (_, lang, code) => {
    let cleanCode = code;
    if (cleanCode.endsWith('\n')) {
      cleanCode = cleanCode.slice(0, -1);
    }
    const normalizedLang = (lang || inferCodeLanguage(cleanCode)).toLowerCase();
    const isCSharp = /\b(using\s+System|namespace\s+[A-Za-z_]|Console\.(Write|Read)|\b(public|private|internal)\s+(class|interface|record))\b/.test(cleanCode);
    const displayLang = isCSharp && (normalizedLang === 'c' || normalizedLang === 'cpp' || normalizedLang === 'code') ?
        'csharp' : normalizedLang;
    codeBlocks.push({lang: displayLang, code: cleanCode});
    return `\n\n%%%CODE_BLOCK_${codeBlocks.length - 1}%%%\n\n`;
  });

  // 3. Escape raw HTML entities
  src = src.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');

  // 4. Block-level transforms (headings, blockquotes)
  src = src.replace(/^###[ \t]+([^\n]+)$/gm, '<h5 class="md-heading">$1</h5>');
  src = src.replace(/^##[ \t]+([^\n]+)$/gm, '<h4 class="md-heading">$1</h4>');
  src = src.replace(/^#[ \t]+([^\n]+)$/gm, '<h3 class="md-heading">$1</h3>');
  src = src.replace(/^&gt;[ \t]+([^\n]+)$/gm, '<blockquote class="md-blockquote">$1</blockquote>');

  // 5. Consecutive lists grouped into proper container tags
  src = src.replace(/(?:^[ \t]*[-*][ \t]+[^\n]+(?:\n|$))+/gm, (match) => {
    const items = match.trim().split('\n').map(line => {
      const itemText = line.replace(/^[ \t]*[-*][ \t]+/, '');
      return `<li>${itemText}</li>`;
    }).join('');
    return `<ul class="md-list">${items}</ul>\n`;
  });

  src = src.replace(/(?:^[ \t]*\d+\.[ \t]+[^\n]+(?:\n|$))+/gm, (match) => {
    const items = match.trim().split('\n').map(line => {
      const itemText = line.replace(/^[ \t]*\d+\.[ \t]+/, '');
      return `<li>${itemText}</li>`;
    }).join('');
    return `<ol class="md-list">${items}</ol>\n`;
  });

  // 6. Inline transforms
  src = src.replace(/`([^`\n]+)`/g, '<code class="md-inline-code">$1</code>');
  src = src.replace(/\*\*([^*\n]+)\*\*/g, '<strong>$1</strong>');
  src = src.replace(/\*([^*\n]+)\*/g, '<em>$1</em>');
  src = src.replace(/\[([^\]\n]+)\]\((https?:\/\/[^\)\s]+)\)/g, '<a href="$2" target="_blank" rel="noreferrer">$1</a>');

  // 7. Split into blocks and wrap non-block text in valid <p>
  const rawParagraphs = src.split(/\n\n+/).map(p => p.trim()).filter(Boolean);
  const formattedParagraphs: string[] = [];

  for (const block of rawParagraphs) {
    if (block.startsWith('<h') || block.startsWith('<blockquote') || block.startsWith('%%%CODE_BLOCK_')) {
      formattedParagraphs.push(block);
    } else if (block.includes('<ul') || block.includes('<ol>')) {
      const parts = block.split(/(<[uo]l class="md-list">[\s\S]*?<\/[uo]l>)/g).map(s => s.trim()).filter(Boolean);
      for (const part of parts) {
        if (part.startsWith('<ul') || part.startsWith('<ol>')) {
          formattedParagraphs.push(part);
        } else {
          formattedParagraphs.push(`<p>${part.replace(/\n/g, '<br>')}</p>`);
        }
      }
    } else {
      formattedParagraphs.push(`<p>${block.replace(/\n/g, '<br>')}</p>`);
    }
  }

  let output = formattedParagraphs.join('\n');

  // 8. Re-insert syntax highlighted code blocks
  output = output.replace(/%%%CODE_BLOCK_(\d+)%%%/g, (_, idx) => {
    const block = codeBlocks[parseInt(idx, 10)];
    if (!block) {
      return '';
    }
    const {lang, code} = block;
    const hl = highlightCode(code, lang);
    const escCode = code.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
    const safeLang = lang.replace(/[^a-zA-Z0-9_+.#-]/g, '');
    return `<div class="md-code-block"><div class="md-code-header"><span class="md-code-lang">${safeLang}</span><button class="md-copy-btn" data-code="${escCode}">Copy</button></div><pre><code>${hl}</code></pre></div>`;
  });

  return output;
}

let safeHtmlPolicy: TrustedTypePolicy | null = null;
try {
  if (window.trustedTypes) {
    safeHtmlPolicy = window.trustedTypes.defaultPolicy || window.trustedTypes.createPolicy('ark-html-policy', {
      createHTML: (s: string) => s,
      createScript: () => '',
      createScriptURL: () => '',
    });
  }
} catch {
  // If policy cannot be created or already exists
}

function setSafeHTML(element: HTMLElement, html: string): void {
  if (safeHtmlPolicy) {
    try {
      element.innerHTML = safeHtmlPolicy.createHTML(html);
      return;
    } catch {
      // Fall through to DOMParser
    }
  }

  // Use DOMParser to parse HTML into nodes and insert with replaceChildren,
  // completely avoiding any TrustedHTML violations.
  try {
    const parser = new DOMParser();
    const doc = parser.parseFromString(html, 'text/html');
    element.replaceChildren(...Array.from(doc.body.childNodes));
    return;
  } catch {
    element.textContent = html;
  }
}

function formatMessageTimestamp(timestamp?: number | bigint): string {
  const ts = typeof timestamp === 'bigint' ? Number(timestamp) : (timestamp || Date.now());
  const date = new Date(ts);
  if (isNaN(date.getTime())) {
    return '';
  }
  const now = new Date();
  const isToday = date.toDateString() === now.toDateString();
  const timeStr = date.toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
  if (isToday) {
    return `Today, ${timeStr}`;
  }
  const dateStr = date.toLocaleDateString([], { month: 'short', day: 'numeric' });
  return `${dateStr}, ${timeStr}`;
}

function renderMessageBubble(
    role: string,
    content: string,
    modelBadge = '',
    timestamp?: number | bigint): HTMLElement {
  const chatPage = get('chat-page');
  const chatMessages = get('chat-messages');
  chatPage.classList.add('has-messages');
  get('chat-empty').hidden = true;
  chatMessages.hidden = false;

  const msg = document.createElement('div');
  msg.className = `message message-${role}`;

  if (role === 'assistant') {
    const sender = document.createElement('div');
    sender.className = 'message-sender';
    const dot = document.createElement('span');
    dot.className = 'status-dot active';
    const badge = document.createElement('span');
    badge.className = 'model-badge';
    badge.textContent = modelBadge || getModelDisplayName(activeModel);
    sender.append(dot, badge);
    msg.appendChild(sender);
  }

  const bubble = document.createElement('div');
  bubble.className = 'message-bubble';
  if (role === 'assistant' || role === 'user') {
    setSafeHTML(bubble, renderMarkdown(content));
    attachCopyHandlers(bubble);
  } else {
    bubble.textContent = content;
  }
  msg.appendChild(bubble);

  const timeEl = document.createElement('time');
  timeEl.className = 'message-time';
  timeEl.textContent = formatMessageTimestamp(timestamp);
  msg.appendChild(timeEl);

  chatMessages.appendChild(msg);
  chatMessages.scrollTop = chatMessages.scrollHeight;
  return bubble;
}

function renderGeneratingBubble(
    targetConversationId: string, modelName: string): HTMLElement {
  const bubble = renderMessageBubble('assistant', '', modelName, Date.now());
  bubble.dataset['generationConversationId'] = targetConversationId;
  renderTypingIndicator(bubble);
  return bubble;
}

async function loadMessages(): Promise<void> {
  if (!conversationId) {
    return;
  }
  const requestedConversationId = conversationId;
  try {
    const {messages}: {messages: ChatMessage[]} =
        await pageHandler.getMessages(requestedConversationId);
    if (conversationId !== requestedConversationId || freshChat) {
      return;
    }
    const persistedMessages = messages ? [...messages] : [];
    const pendingUserMessage = pendingUserMessages.get(requestedConversationId);
    if (pendingUserMessage && !persistedMessages.some(
        message => message.role === 'user' &&
            message.content === pendingUserMessage.content)) {
      persistedMessages.push(pendingUserMessage);
    }
    currentMessages = persistedMessages;
    const chatPage = get('chat-page');
    const chatMessages = get('chat-messages');
    const chatEmpty = get('chat-empty');
    if (persistedMessages.length === 0) {
      chatMessages.replaceChildren();
      if (generatingConversationIds.has(requestedConversationId)) {
        chatPage.classList.add('has-messages');
        chatEmpty.hidden = true;
        chatMessages.hidden = false;
        const generatingConversation = conversationsList.find(
            item => item.id === requestedConversationId);
        renderGeneratingBubble(
            requestedConversationId,
            getModelDisplayName(generatingConversation?.modelName || activeModel));
      } else {
        chatPage.classList.remove('has-messages');
        chatEmpty.hidden = false;
        chatMessages.hidden = true;
      }
    } else {
      chatPage.classList.add('has-messages');
      chatEmpty.hidden = true;
      chatMessages.hidden = false;
      chatMessages.replaceChildren();
      for (const m of persistedMessages) {
        const badge = m.role === 'assistant'
            ? (getModelDisplayName(m.modelName) || getModelDisplayName(activeModel))
            : '';
        renderMessageBubble(m.role, m.content, badge, m.createdAt);
      }
      if (generatingConversationIds.has(requestedConversationId)) {
        const generatingConversation = conversationsList.find(
            item => item.id === requestedConversationId);
        renderGeneratingBubble(
            requestedConversationId,
            getModelDisplayName(generatingConversation?.modelName || activeModel));
      }
      chatMessages.scrollTop = chatMessages.scrollHeight;
    }
  } catch (err) {
    console.error('Failed to load conversation messages:', err);
  }
}

async function loadConversations(): Promise<void> {
  const container = document.getElementById('conversation-list');
  if (!container) {
    return;
  }
  try {
    const {conversations} = await pageHandler.getConversations();
    conversationsList = conversations || [];
    renderConversationList();
  } catch (err) {
    console.warn('Failed to load conversations:', err);
  }
}

function renderConversationList(): void {
  const container = document.getElementById('conversation-list');
  if (!container) {
    return;
  }
  container.replaceChildren();

  if (conversationsList.length === 0) {
    const emptyHint = document.createElement('div');
    emptyHint.className = 'conversation-empty-hint';
    emptyHint.textContent = 'No chats yet';
    container.appendChild(emptyHint);
    return;
  }

  for (const conv of conversationsList) {
    const item = document.createElement('div');
    item.className = 'conversation-item';
    item.dataset['conversationId'] = conv.id;
    item.setAttribute('role', 'listitem');
    if (currentRoute === 'chat' && conv.id === conversationId) {
      item.classList.add('active');
    }
    if (generatingConversationIds.has(conv.id)) {
      item.classList.add('generating');
      item.setAttribute('aria-busy', 'true');
    }

    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'conversation-item-btn';
    btn.title = conv.title || 'New conversation';

    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    icon.setAttribute('class', 'mat-icon mat-icon-sm');
    icon.setAttribute('aria-hidden', 'true');
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', '#mat-chat');
    icon.appendChild(use);

    const titleSpan = document.createElement('span');
    titleSpan.className = 'conversation-item-title';
    titleSpan.textContent = conv.title || 'New conversation';

    btn.append(icon, titleSpan);
    btn.addEventListener('click', () => void switchToConversation(conv.id));

    const delBtn = document.createElement('button');
    delBtn.type = 'button';
    delBtn.className = 'conversation-delete-btn';
    delBtn.title = 'Delete conversation';
    delBtn.setAttribute('aria-label', `Delete conversation ${conv.title || ''}`);
    delBtn.disabled = generatingConversationIds.has(conv.id);
    if (delBtn.disabled) {
      delBtn.title = 'Wait for this response to finish before deleting';
    }

    const delIcon = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    delIcon.setAttribute('class', 'mat-icon mat-icon-sm');
    delIcon.setAttribute('aria-hidden', 'true');
    const delUse = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    delUse.setAttribute('href', '#mat-delete');
    delIcon.appendChild(delUse);

    delBtn.appendChild(delIcon);
    delBtn.addEventListener('click', (e) => {
      e.stopPropagation();
      void deleteConversation(conv.id);
    });

    item.append(btn, delBtn);
    container.appendChild(item);
  }
}

async function switchToConversation(id: string): Promise<void> {
  const switchEpoch = ++conversationStateEpoch;
  try {
    const {state} = await pageHandler.switchConversation(id);
    if (switchEpoch !== conversationStateEpoch || freshChat) {
      return;
    }
    if (state && state.id) {
      freshChat = false;
      conversationId = state.id;
      draft.value = state.draft || '';
      clearButton.disabled = !draft.value;
      updateSendButtonState();
      get('draft-status').textContent = '';
      if (state.modelName) {
        updateModelStatusUI(state.modelName);
        setModelMenuValues(state.modelName);
      }
      if (location.hash !== '#chat') {
        location.hash = 'chat';
        renderRoute(false);
      }
      renderConversationList();
      await loadMessages();
      draft.focus();
    }
  } catch (err) {
    console.warn('Failed to switch conversation:', err);
  }
}

async function deleteConversation(id: string): Promise<void> {
  if (generatingConversationIds.has(id)) {
    showArkNotice(
        'Response in progress',
        'Wait for this conversation to finish generating before deleting it.');
    return;
  }
  try {
    await pageHandler.deleteConversation(id);
    conversationsList = conversationsList.filter(c => c.id !== id);
    if (conversationId === id) {
      if (conversationsList.length > 0) {
        await switchToConversation(conversationsList[0]!.id);
      } else {
        await createNewChat();
      }
    } else {
      renderConversationList();
    }
  } catch (err) {
    console.warn('Failed to delete conversation:', err);
  }
}

function createNewChat(): void {
  // Keep an empty new chat transient. The backend conversation is created by
  // sendMessage() only when the first prompt is submitted.
  freshChat = true;
  conversationStateEpoch++;
  conversationId = '';
  draft.value = '';
  saveDraft();
  updateSendButtonState();
  currentMessages = [];
  const chatMessages = get('chat-messages');
  chatMessages.replaceChildren();
  chatMessages.hidden = true;
  get('chat-page').classList.remove('has-messages');
  get('chat-empty').hidden = false;
  if (location.hash !== '#chat') {
    location.hash = 'chat';
    renderRoute(false);
  }
  renderConversationList();
  draft.focus();
}

async function streamTextToBubble(fullText: string, bubble: HTMLElement): Promise<string> {
  bubble.replaceChildren();
  const chatMessages = get('chat-messages');
  const tokens = fullText.split(/(\s+)/);
  let current = '';

  for (let i = 0; i < tokens.length; i++) {
    current += tokens[i];
    setSafeHTML(bubble, renderMarkdown(current));
    chatMessages.scrollTop = chatMessages.scrollHeight;
    if (i % 2 === 0) {
      await new Promise(r => setTimeout(r, 14));
    }
  }
  setSafeHTML(bubble, renderMarkdown(fullText));
  attachCopyHandlers(bubble);
  chatMessages.scrollTop = chatMessages.scrollHeight;
  return fullText;
}

async function generateResponse(
    targetConversationId: string, prompt: string,
    modelName: string): Promise<string> {
  const providerCredential = modelName.startsWith('cloud:gemini') ?
      (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim() : null;
  const result = await pageHandler.sendChatPrompt(
      targetConversationId, prompt, null, providerCredential);
  if (!result.success) throw new Error(result.response || 'Model request failed.');
  const text = result.response || '';
  if (!text) throw new Error('The model returned an empty response.');
  return text;
}

async function generateAndApplyConversationTitle(
    targetConversationId: string, userMessage: string,
    modelName: string): Promise<void> {
  if (!targetConversationId) return;
  const providerCredential = modelName.startsWith('cloud:gemini') ?
      (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim() : null;
  const result = await pageHandler.generateConversationTitle(
      targetConversationId, userMessage, providerCredential);
  const title = (result.title || generateChatTitle(userMessage)).trim();
  const conversation = conversationsList.find(
      item => item.id === targetConversationId);
  if (conversation) conversation.title = title;
  await pageHandler.updateConversationTitle(targetConversationId, title);
  await loadConversations();
  const titleElement = document.querySelector<HTMLElement>(
      `.conversation-item[data-conversation-id="${CSS.escape(targetConversationId)}"] .conversation-item-title`);
  if (!titleElement) return;
  titleElement.textContent = '';
  for (const character of title) {
    titleElement.textContent += character;
    await new Promise(resolve => setTimeout(resolve, 18));
  }
}

function generateChatTitle(firstPrompt: string): string {
  const clean = firstPrompt.trim().replace(/\s+/g, ' ');
  if (!clean) {
    return 'New conversation';
  }
  const maxLen = 38;
  if (clean.length <= maxLen) {
    return clean;
  }
  const truncated = clean.slice(0, maxLen);
  const lastSpace = truncated.lastIndexOf(' ');
  if (lastSpace > 16) {
    return truncated.slice(0, lastSpace) + '...';
  }
  return truncated + '...';
}

async function sendMessage(overrideText?: string): Promise<void> {
  if (conversationId && generatingConversationIds.has(conversationId)) {
    return;
  }
  const text = (overrideText ?? draft.value).trim();
  if (!text) {
    return;
  }

  const isInstalled = installedLocalModels.some(
      model => model.modelId === activeModel && isSelectableLocalModel(model));
  const geminiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  const hasGemini = Boolean(geminiKey);

  if (activeModel.startsWith('local:') && !isInstalled) {
    showArkNotice(
        'Local model required',
        'Download and select a compatible model from the Models page first.');
    location.hash = 'models';
    return;
  }
  if (activeModel.startsWith('cloud:gemini') && !hasGemini) {
    showArkNotice(
        'Cloud credentials required',
        'Configure your Gemini API key in Cloud Providers before sending.');
    location.hash = 'models';
    setModelTab(false);
    return;
  }

  if (!conversationId) {
    try {
      const {state} = await pageHandler.createConversation(activeModel);
      conversationId = state?.id || crypto.randomUUID();
      if (state?.id && !conversationsList.some(item => item.id === state.id)) {
        conversationsList.unshift(state);
      }
    } catch {
      conversationId = crypto.randomUUID();
    }
    freshChat = false;
  }

  const requestConversationId = conversationId;
  const requestModel = activeModel;
  const isFirstMessage = currentMessages.filter(m => m.role === 'user').length === 0;
  const now = Date.now();
  const pendingUserMessage: ChatMessage = {
    id: BigInt(now),
    conversationId: requestConversationId,
    role: 'user',
    content: text,
    createdAt: BigInt(now),
    modelName: requestModel,
  };
  pendingUserMessages.set(requestConversationId, pendingUserMessage);
  generatingConversationIds.add(requestConversationId);
  draft.value = '';
  saveDraft();
  updateSendButtonState();

  if (location.hash !== '#chat') {
    location.hash = 'chat';
    renderRoute(false);
  }

  get('chat-page').classList.add('has-messages');
  renderMessageBubble('user', text, '', now);
  currentMessages.push(pendingUserMessage);
  updateSendButtonState();
  renderConversationList();
  const badgeName = getModelDisplayName(requestModel);
  renderGeneratingBubble(requestConversationId, badgeName);

  try {
    const reply = await generateResponse(
        requestConversationId, text, requestModel);
    if (reply) {
      const visibleBubble = document.querySelector<HTMLElement>(
          `.message-bubble[data-generation-conversation-id="${CSS.escape(requestConversationId)}"]`);
      if (visibleBubble && conversationId === requestConversationId &&
          !freshChat) {
        await streamTextToBubble(reply, visibleBubble);
      }
      const replyNow = Date.now();
      if (conversationId === requestConversationId && !freshChat) {
        currentMessages.push({
          id: BigInt(replyNow),
          conversationId: requestConversationId,
          role: 'assistant',
          content: reply,
          createdAt: BigInt(replyNow),
          modelName: requestModel,
        });
      }
      generatingConversationIds.delete(requestConversationId);
      updateSendButtonState();
      renderConversationList();
      if (isFirstMessage) {
        await generateAndApplyConversationTitle(
            requestConversationId, text, requestModel);
      } else {
        void loadConversations();
      }
    }
  } catch (e: unknown) {
    const msg = e instanceof Error ? e.message : String(e);
    const visibleBubble = document.querySelector<HTMLElement>(
        `.message-bubble[data-generation-conversation-id="${CSS.escape(requestConversationId)}"]`);
    if (visibleBubble && conversationId === requestConversationId &&
        !freshChat) {
      visibleBubble.textContent = `Error: ${msg}`;
    }
  } finally {
    pendingUserMessages.delete(requestConversationId);
    generatingConversationIds.delete(requestConversationId);
    updateSendButtonState();
    renderConversationList();
    if (conversationId === requestConversationId && !freshChat) {
      await loadMessages();
      draft.focus();
    }
  }
}

const sendButton = document.querySelector<HTMLButtonElement>('.send-button');
sendButton?.addEventListener('click', () => {
  void sendMessage();
});

async function loadChatState(): Promise<void> {
  if (freshChat) {
    return;
  }
  if (conversationId) {
    document.documentElement.dataset['storageReady'] = 'true';
    void loadConversations();
    updateSendButtonState();
    return;
  }
  const stateEpoch = conversationStateEpoch;
  try {
    const {state} = await pageHandler.getChatState();
    if (freshChat || stateEpoch !== conversationStateEpoch) {
      return;
    }
    if (state && state.id) {
      conversationId = state.id;
      document.documentElement.dataset['storageReady'] = 'true';
      if (document.activeElement !== draft) {
        draft.value = state.draft.slice(0, 32000);
        clearButton.disabled = !draft.value;
      }
      if (state.modelName) {
        updateModelStatusUI(state.modelName);
        setModelMenuValues(state.modelName);
      }
      void loadMessages();
      void loadConversations();
      updateSendButtonState();
      get('draft-status').textContent = '';
      return;
    }
  } catch (err) {
    console.warn('Failed to load chat state:', err);
  }
  if (!conversationId) {
    conversationId = crypto.randomUUID();
  }
  document.documentElement.dataset['storageReady'] = 'true';
  void loadConversations();
  updateSendButtonState();
  get('draft-status').textContent = '';
}
void loadChatState();
window.addEventListener('focus', () => void loadChatState());
document.addEventListener('visibilitychange', () => {
  if (!document.hidden) {
    void loadChatState();
  }
});

const visitedKey = 'ark.has_opened';
let hasOpened = false;
try {
  hasOpened = Boolean(localStorage.getItem(visitedKey));
  if (!hasOpened) {
    localStorage.setItem(visitedKey, 'true');
  }
} catch {
  // Local storage unavailable in private or restricted contexts.
}

if (hasOpened) {
  const eyebrow = document.getElementById('home-eyebrow');
  const title = document.getElementById('home-title');
  const tagline = document.getElementById('home-tagline');
  if (eyebrow && title && tagline) {
    eyebrow.textContent = 'A FRESH START';
    title.textContent = 'Where to next';
    const accent = document.createElement('span');
    accent.className = 'accent';
    accent.textContent = '?';
    title.append(accent);
    tagline.textContent = 'Follow a thought. Find something good.';
  }
}

function renderRoute(focus: boolean): void {
  const requested = location.hash.slice(1) || 'home';
  currentRoute = routes.includes(requested as Route) ? requested as Route : 'home';
  for (const route of routes) {
    get(`${route}-page`).hidden = route !== currentRoute;
  }
  document.querySelectorAll<HTMLAnchorElement>('[data-route]').forEach(link => {
    if (link.dataset['route'] === currentRoute) {
      link.setAttribute('aria-current', 'page');
    } else {
      link.removeAttribute('aria-current');
    }
  });
  renderConversationList();
  const composer = get('composer');
  composer.hidden = currentRoute !== 'chat';
  if (!composer.hidden) {
    get('chat-composer-slot').append(composer);
  }
  get('breadcrumb').textContent = currentRoute === 'home' ?
      'Your everyday, with more possibility.' :
      `Your workspace / ${currentRoute === 'advanced' ? 'Advanced options' :
          currentRoute.charAt(0).toUpperCase() + currentRoute.slice(1)}`;
  document.title = currentRoute === 'home' ? 'New Tab — Ark Browser' :
      `${currentRoute === 'advanced' ? 'Advanced options' :
          currentRoute.charAt(0).toUpperCase() + currentRoute.slice(1)} — Ark Browser`;
  if (currentRoute === 'models') {
    void refreshLocalModelState();
  }
  if (currentRoute === 'chat') {
    void loadMessages();
    updateSendButtonState();
  }
  if (focus) {
    get('main').focus();
    window.scrollTo(0, 0);
  }
}
window.addEventListener('hashchange', () => renderRoute(true));
if (isSidePanel && location.hash !== '#chat') {
  location.hash = 'chat';
} else {
  renderRoute(false);
}

get('new-chat').addEventListener('click', async () => {
  if (draft.value) {
    newChatPending = true;
    dialog.showModal();
  } else {
    await createNewChat();
  }
});
clearButton.addEventListener('click', () => {
  newChatPending = false;
  dialog.showModal();
});
dialog.addEventListener('close', async () => {
  if (dialog.returnValue === 'clear') {
    draft.value = '';
    saveDraft();
    get('draft-status').textContent = 'Draft cleared.';
    if (newChatPending) {
      await createNewChat();
    }
  }
  newChatPending = false;
  draft.focus();
});
// Escape must not reuse a previous affirmative dialog result.
dialog.addEventListener('cancel', () => { dialog.returnValue = 'cancel'; });

document.querySelectorAll<HTMLButtonElement>('[data-prompt]').forEach(button => {
  button.addEventListener('click', () => {
    if (draft.value) {
      get('draft-status').textContent =
          'Your draft is already started. Clear it to use a suggestion.';
    } else {
      draft.value = button.dataset['prompt'] || '';
      saveDraft();
      get('draft-status').textContent = 'Suggestion added to your draft. Nothing was sent.';
    }
    draft.focus();
    draft.setSelectionRange(draft.value.length, draft.value.length);
  });
});

// Appearance syncs automatically with browser / system theme.
delete document.documentElement.dataset['theme'];
try {
  sessionStorage.removeItem('ark.preview.theme.v1');
} catch {
  // Appearance still works when tab-session storage is unavailable.
}

const searchInput = get<HTMLInputElement>('search-input');
const searchForm = get<HTMLFormElement>('search-form');
const searchBoxWrapper = get<HTMLDivElement>('search-box-wrapper');
const searchSuggestions = get<HTMLDivElement>('search-suggestions');

let suggestionsList: string[] = [];
let selectedSuggestionIndex = -1;
let originalUserQuery = '';
let suggestDebounceTimer = 0;

function hideSuggestions(): void {
  searchSuggestions.hidden = true;
  searchSuggestions.replaceChildren();
  searchBoxWrapper.classList.remove('has-suggestions');
  selectedSuggestionIndex = -1;
}

function selectSuggestion(index: number): void {
  const items = searchSuggestions.querySelectorAll<HTMLButtonElement>('.suggestion-item');
  items.forEach((item, i) => {
    const isSelected = i === index;
    item.classList.toggle('active', isSelected);
    item.setAttribute('aria-selected', String(isSelected));
  });
  selectedSuggestionIndex = index;
  if (index >= 0 && index < suggestionsList.length) {
    searchInput.value = suggestionsList[index]!;
  } else {
    searchInput.value = originalUserQuery;
  }
}

function submitSearch(query: string): void {
  searchInput.value = query;
  hideSuggestions();
  searchForm.requestSubmit();
}

function renderSuggestions(query: string, suggestions: string[]): void {
  suggestionsList = suggestions;
  selectedSuggestionIndex = -1;
  searchSuggestions.replaceChildren();

  if (suggestions.length === 0 || aiSearchMode) {
    hideSuggestions();
    return;
  }

  const queryLower = query.toLowerCase();
  for (let i = 0; i < suggestions.length; i++) {
    const text = suggestions[i]!;
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.className = 'suggestion-item';
    btn.setAttribute('role', 'option');
    btn.setAttribute('aria-selected', 'false');

    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    svg.setAttribute('class', 'mat-icon suggestion-icon');
    svg.setAttribute('aria-hidden', 'true');
    const use = document.createElementNS('http://www.w3.org/2000/svg', 'use');
    use.setAttribute('href', '#mat-search');
    svg.appendChild(use);

    const textSpan = document.createElement('span');
    textSpan.className = 'suggestion-text';

    if (text.toLowerCase().startsWith(queryLower)) {
      const matchSpan = document.createElement('span');
      matchSpan.className = 'suggestion-match';
      matchSpan.textContent = text.slice(0, query.length);
      const rest = document.createTextNode(text.slice(query.length));
      textSpan.append(matchSpan, rest);
    } else {
      textSpan.textContent = text;
    }

    btn.append(svg, textSpan);

    btn.addEventListener('mouseenter', () => {
      selectSuggestion(i);
    });

    btn.addEventListener('pointerdown', (e) => {
      e.preventDefault();
      submitSearch(text);
    });

    searchSuggestions.appendChild(btn);
  }

  searchSuggestions.hidden = false;
  searchBoxWrapper.classList.add('has-suggestions');
}

function fetchSuggestions(query: string): void {
  window.clearTimeout(suggestDebounceTimer);
  const trimmed = query.trim();
  if (aiSearchMode || !trimmed) {
    hideSuggestions();
    return;
  }
  suggestDebounceTimer = window.setTimeout(async () => {
    const currentInput = searchInput.value.trim();
    if (aiSearchMode || !currentInput) {
      hideSuggestions();
      return;
    }
    try {
      const {suggestions} = await pageHandler.getSearchSuggestions(currentInput);
      if (searchInput.value.trim().toLowerCase() === currentInput.toLowerCase()) {
        renderSuggestions(currentInput, suggestions);
      }
    } catch (err) {
      console.error('Failed to fetch search suggestions:', err);
      hideSuggestions();
    }
  }, 100);
}

searchInput.addEventListener('input', () => {
  originalUserQuery = searchInput.value;
  selectedSuggestionIndex = -1;
  fetchSuggestions(searchInput.value);
});

searchInput.addEventListener('focus', () => {
  if (!aiSearchMode && searchInput.value.trim()) {
    fetchSuggestions(searchInput.value);
  }
});

searchInput.addEventListener('keydown', (e: KeyboardEvent) => {
  if (e.key === 'ArrowDown') {
    if (searchSuggestions.hidden) {
      if (suggestionsList.length > 0) {
        searchSuggestions.hidden = false;
        searchBoxWrapper.classList.add('has-suggestions');
      } else {
        return;
      }
    }
    e.preventDefault();
    const next = selectedSuggestionIndex < suggestionsList.length - 1 ?
        selectedSuggestionIndex + 1 : -1;
    selectSuggestion(next);
  } else if (e.key === 'ArrowUp') {
    if (searchSuggestions.hidden) {
      return;
    }
    e.preventDefault();
    const prev = selectedSuggestionIndex === -1 ?
        suggestionsList.length - 1 : selectedSuggestionIndex - 1;
    selectSuggestion(prev);
  } else if (e.key === 'Escape') {
    if (!searchSuggestions.hidden) {
      e.preventDefault();
      searchInput.value = originalUserQuery;
      hideSuggestions();
    }
  }
});

document.addEventListener('pointerdown', (e: PointerEvent) => {
  if (!searchBoxWrapper.contains(e.target as Node)) {
    hideSuggestions();
  }
});

function setSearchMode(ai: boolean): void {
  aiSearchMode = ai;
  hideSuggestions();
  get('web-mode').setAttribute('aria-pressed', String(!ai));
  get('ai-mode').setAttribute('aria-pressed', String(ai));
  get('home-ai-toolbar').hidden = !ai;
  searchInput.placeholder = ai ? 'Ask AI anything' : 'Search the web or enter an address';
  searchInput.setAttribute('aria-label', ai ? 'Ask AI' : 'Search the web or enter an address');
  searchInput.focus();
}
get('web-mode').addEventListener('click', () => setSearchMode(false));
get('ai-mode').addEventListener('click', () => setSearchMode(true));

searchForm.addEventListener('submit', async event => {
  event.preventDefault();
  const query = searchInput.value.trim();
  hideSuggestions();
  if (!query) {
    return;
  }
  get('search-status').textContent = '';
  try {
    if (aiSearchMode) {
      // Ask AI always starts a transient chat. This prevents a stale
      // previously selected conversation from receiving the first prompt and
      // lets sendMessage create the row on demand.
      createNewChat();
      void sendMessage(query);
      return;
    }
    const {success} = await pageHandler.navigate(query);
    if (!success) {
      throw new Error('Navigation rejected');
    }
  } catch {
    get('search-status').textContent =
        'This address could not be opened. Try the browser’s address bar.';
  }
});

function setModelTab(local: boolean): void {
  get('local-models').hidden = !local;
  get('cloud-models').hidden = local;
  get('local-tab').setAttribute('aria-pressed', String(local));
  get('cloud-tab').setAttribute('aria-pressed', String(!local));
}
get('local-tab').addEventListener('click', () => setModelTab(true));
get('cloud-tab').addEventListener('click', () => setModelTab(false));

function textElement<K extends keyof HTMLElementTagNameMap>(
    tag: K, text: string, className = ''): HTMLElementTagNameMap[K] {
  const element = document.createElement(tag);
  element.textContent = text;
  element.className = className;
  return element;
}

const providers = [
  ['Google AI Studio (Gemini)', 'Connect with your Gemini API key from Google AI Studio or Google Cloud Vertex AI to run models like Gemini 2.5 Pro and Gemini 2.5 Flash.'],
  ['OpenAI', 'Connect with your own API key and choose a model.'],
  ['Anthropic', 'Use Claude through your own Anthropic connection.'],
  ['Amazon Bedrock', 'Choose an AWS region, credential source, and model.'],
  ['Together AI', 'Connect to models hosted by Together AI.'],
  ['Custom endpoint', 'Bring an OpenAI-compatible endpoint and model identifier.'],
];
for (const [name, description] of providers) {
  const card = document.createElement('article');
  card.className = 'provider-card';
  const title = document.createElement('div');
  title.className = 'provider-title';

  if (name === 'Google AI Studio (Gemini)') {
    const savedKey = localStorage.getItem(GEMINI_KEY_STORAGE) || '';
    const statusTag = textElement(
        'span', savedKey ? 'Configured' : 'Available',
        savedKey ? 'tag tag-success' : 'tag tag-prepared');
    title.append(textElement('h3', name), statusTag);
    card.append(title, textElement('p', description!));

    const keyForm = document.createElement('div');
    keyForm.className = 'provider-key-form';

    const keyRow = document.createElement('div');
    keyRow.className = 'provider-key-row';

    const keyInput = document.createElement('input');
    keyInput.type = 'password';
    keyInput.className = 'provider-key-input';
    keyInput.placeholder = 'Paste Gemini API key (AIzaSy…)';
    keyInput.value = savedKey;

    const saveBtn = document.createElement('button');
    saveBtn.type = 'button';
    saveBtn.className = 'button-sm';
    saveBtn.textContent = savedKey ? 'Update' : 'Save';

    const removeBtn = document.createElement('button');
    removeBtn.type = 'button';
    removeBtn.className = 'button-sm button-danger';
    removeBtn.textContent = 'Remove';
    removeBtn.hidden = !savedKey;

    keyRow.append(keyInput, saveBtn, removeBtn);

    const metaRow = document.createElement('div');
    metaRow.className = 'provider-key-meta';

    const statusLabel = document.createElement('span');
    statusLabel.className = 'provider-key-status' + (savedKey ? ' saved' : '');
    statusLabel.textContent = savedKey ? 'Key stored locally in Ark' : 'No API key configured';

    const link = document.createElement('a');
    link.href = 'https://aistudio.google.com/app/apikey';
    link.target = '_blank';
    link.rel = 'noreferrer';
    link.className = 'text-link';
    link.textContent = 'Get Gemini API key';

    metaRow.append(statusLabel, link);
    keyForm.append(keyRow, metaRow);
    card.append(keyForm);

    saveBtn.addEventListener('click', () => {
      const val = keyInput.value.trim();
      if (!val) {
        statusLabel.textContent = 'Please enter an API key first.';
        statusLabel.classList.remove('saved');
        return;
      }
      localStorage.setItem(GEMINI_KEY_STORAGE, val);
      statusTag.textContent = 'Configured';
      statusTag.className = 'tag tag-success';
      statusLabel.textContent = 'Key stored locally in Ark';
      statusLabel.classList.add('saved');
      saveBtn.textContent = 'Update';
      removeBtn.hidden = false;
      refreshModelMenus();
    });

    removeBtn.addEventListener('click', () => {
      localStorage.removeItem(GEMINI_KEY_STORAGE);
      keyInput.value = '';
      statusTag.textContent = 'Available';
      statusTag.className = 'tag tag-prepared';
      statusLabel.textContent = 'No API key configured';
      statusLabel.classList.remove('saved');
      saveBtn.textContent = 'Save';
      removeBtn.hidden = true;
      refreshModelMenus();
    });
  } else {
    title.append(textElement('h3', name!), textElement('span', 'Coming soon', 'tag'));
    card.append(title, textElement('p', description!));
  }

  get('provider-grid').append(card);
}

const parameterGroups: Array<[string, Array<[string, string]>]> = [
  ['Generation', [
    ['Temperature', 'Controls randomness. Lower values tend to give more consistent answers; higher values allow more variety.'],
    ['Top P', 'Limits choices to tokens covering a probability mass. Lower values narrow the choices.'],
    ['Top K', 'Limits each next-token choice to the K most likely tokens, where supported.'],
    ['Maximum output tokens', 'Caps the tokens generated for one response. Reasoning may share this budget, depending on the model.'],
    ['Repetition penalty', 'Discourages repeating tokens that already appeared. The scale depends on the runtime.'],
    ['Frequency penalty', 'Discourages tokens in proportion to how often they have appeared.'],
    ['Presence penalty', 'Discourages reusing tokens that have already appeared.'],
    ['Stop sequences', 'Ends generation when one of the chosen text sequences is produced.'],
    ['Seed', 'Requests repeatable sampling when supported. Results can still vary across hardware and model versions.'],
    ['Reasoning effort / budget', 'Adjusts how much work a supported reasoning model spends before answering. This can affect latency and usage.'],
  ]],
  ['Instructions', [
    ['System instructions', 'Sets the assistant’s preferred behavior. Does not grant browser permissions or change context rules.'],
  ]],
  ['Connection', [
    ['Request timeout', 'Sets how long Ark waits before ending an unresponsive request.'],
  ]],
  ['Local performance · local models only', [
    ['Context window', 'Sets how much text a local model can hold while generating an answer. Larger values use more memory. Cloud limits are usually read-only.'],
    ['GPU offload', 'Moves supported parts of a local model to the GPU, using GPU memory.'],
    ['CPU threads', 'Sets how many CPU threads local inference can use.'],
    ['Batch size', 'Controls how many input tokens a local model processes together. Larger batches can use more memory.'],
    ['Idle unload delay', 'Releases an unused local model from memory. Never unloads during generation.'],
  ]],
];
for (const [name, parameters] of parameterGroups) {
  const section = document.createElement('section');
  section.className = 'parameter-section';
  section.append(textElement('h2', name));
  for (const [label, description] of parameters) {
    const row = document.createElement('div');
    row.className = 'parameter-row';
    row.append(textElement('h3', label), textElement('p', description));
    section.append(row);
  }
  get('parameter-groups').append(section);
}

function formatBytes(bytes: number): string {
  if (bytes <= 0) {
    return '0 B';
  }
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  const i = Math.floor(Math.log(bytes) / Math.log(1024));
  return `${(bytes / Math.pow(1024, i)).toFixed(i === 0 ? 0 : 1)} ${units[i]}`;
}

let modelPollTimer = 0;

function updateLocalModelUI(state: LocalModelState): void {
  currentLocalModelState = state;
  get('prepared-model-title').textContent = state.displayName;
  const stateTag = get('model-state-tag');
  const progressWrap = get('model-progress-wrap');
  const progress = get<HTMLProgressElement>('model-progress');
  const progressLabel = get('model-progress-label');
  const progressPercent = get('model-progress-percent');
  const detail = get('model-download-detail');
  const licenseRow = get('model-license-row');
  const downloadBtn = get<HTMLButtonElement>('model-download');
  const pauseBtn = get<HTMLButtonElement>('model-pause');
  const resumeBtn = get<HTMLButtonElement>('model-resume');
  const deleteBtn = get<HTMLButtonElement>('model-delete');

  stateTag.textContent = state.state.charAt(0).toUpperCase() + state.state.slice(1);
  stateTag.dataset['state'] = state.state;

  detail.textContent = state.detail;

  const isDownloading = state.state === 'downloading' || state.state === 'paused' ||
      state.state === 'interrupted' || state.state === 'verifying' ||
      state.state === 'preparing' || state.state === 'starting';
  progressWrap.hidden = !isDownloading && !state.installed;

  if (state.bytesTotal > 0) {
    const percent = Math.min(100, Math.round((state.bytesDownloaded / state.bytesTotal) * 100));
    progress.value = percent;
    progressPercent.textContent = `${percent}%`;
    progressLabel.textContent = `${formatBytes(state.bytesDownloaded)} of ${formatBytes(state.bytesTotal)}`;
  }

  downloadBtn.hidden = !state.canStart;
  pauseBtn.hidden = !state.canPause;
  resumeBtn.hidden = !state.canResume;
  licenseRow.hidden = state.installed || !state.canStart;
  deleteBtn.hidden = !state.installed;

  if (state.installed) {
    stateTag.textContent = 'Installed';
    stateTag.classList.add('tag-success');
    downloadBtn.hidden = true;
    pauseBtn.hidden = true;
    resumeBtn.hidden = true;
    deleteBtn.hidden = false;
  } else {
    stateTag.classList.remove('tag-success');
    deleteBtn.hidden = true;
  }

  refreshModelMenus();
  if (state.installed) {
    void refreshInstalledModels();
  }

  window.clearTimeout(modelPollTimer);
  if (state.state === 'downloading' || state.state === 'preparing' ||
      state.state === 'starting' || state.state === 'verifying') {
    modelPollTimer = window.setTimeout(async () => {
      try {
        const {state: refreshed} = await pageHandler.getLocalModelState();
        updateLocalModelUI(refreshed);
      } catch (e) {
        console.error('Failed to poll model state:', e);
      }
    }, 1000);
  }
}

async function refreshLocalModelState(): Promise<void> {
  try {
    const {state} = await pageHandler.getLocalModelState();
    updateLocalModelUI(state);
  } catch (err) {
    console.error('Failed to load local model state:', err);
  }
}

get('model-download').addEventListener('click', async () => {
  const licenseCheckbox = get<HTMLInputElement>('model-license');
  const detail = get('model-download-detail');
  licenseCheckbox.checked = true;
  detail.textContent = 'Starting download…';
  try {
    const {state} = await pageHandler.startLocalModelDownload(
        'mlx-community/Llama-3.2-11B-Vision-Instruct-4bit', true);
    updateLocalModelUI(state);
  } catch (err) {
    console.error('Failed to start download in Ark:', err);
    detail.textContent = 'Failed to start download in Ark.';
  }
});

get('model-pause').addEventListener('click', async () => {
  try {
    const {state} = await pageHandler.pauseLocalModelDownload();
    updateLocalModelUI(state);
  } catch {
    get('model-download-detail').textContent = 'Failed to pause download.';
  }
});

get('model-resume').addEventListener('click', async () => {
  try {
    const {state} = await pageHandler.resumeLocalModelDownload();
    updateLocalModelUI(state);
  } catch {
    get('model-download-detail').textContent = 'Failed to resume download.';
  }
});

get('model-delete').addEventListener('click', async () => {
  const modelId = currentLocalModelState?.modelId || PREPARED_LOCAL_MODEL;
  const displayName = currentLocalModelState?.displayName || 'this model';
  if (!await confirmModelDelete(displayName)) {
    return;
  }
  const detail = get('model-download-detail');
  detail.textContent = 'Deleting model files…';
  try {
    const {state} = await pageHandler.deleteLocalModel(modelId);
    updateLocalModelUI(state);
    detail.textContent = 'Model deleted from local storage.';
  } catch (err) {
    console.warn('Failed to delete model via remote:', err);
    detail.textContent = 'Model deleted from local storage.';
    await refreshLocalModelState();
  }
  await refreshInstalledModels();
});

const modelSearchForm = get<HTMLFormElement>('model-search-form');
const modelSearchInput = get<HTMLInputElement>('model-search-input');
const modelSearchStatus = get('model-search-status');
const modelSearchResults = get('model-search-results');

modelSearchForm.addEventListener('submit', async (e) => {
  e.preventDefault();
  const query = modelSearchInput.value.trim();
  if (!query) {
    return;
  }
  modelSearchStatus.textContent = 'Searching Hugging Face for MLX and GGUF models…';
  modelSearchResults.replaceChildren();

  try {
    const {results, error} = await pageHandler.searchLocalModels(query);
    if (error) {
      modelSearchStatus.textContent = error;
      return;
    }
    if (results.length === 0) {
      modelSearchStatus.textContent = `No MLX or GGUF models found for "${query}".`;
      return;
    }
    modelSearchStatus.textContent = `Found ${results.length} model${results.length === 1 ? '' : 's'} on Hugging Face:`;
    for (const result of results) {
      const card = document.createElement('div');
      card.className = 'search-result-card';

      const info = document.createElement('div');
      info.className = 'search-result-info';

      const title = document.createElement('strong');
      title.className = 'search-result-title';
      title.textContent = result.id;
      info.appendChild(title);

      const meta = document.createElement('span');
      meta.className = 'search-result-meta';
      meta.textContent = `${result.runtimeBackend === 'mlx-vlm' ? 'MLX (preferred)' : 'GGUF (legacy)'} · ${result.downloads.toLocaleString()} downloads`;
      info.appendChild(meta);

      card.appendChild(info);

      const badges = document.createElement('div');
      badges.className = 'search-result-badges';

      if (result.prepared) {
        const prepBadge = document.createElement('span');
        prepBadge.className = 'tag tag-prepared';
        prepBadge.textContent = 'Prepared for Ark';
        badges.appendChild(prepBadge);
      }

      if (result.gated) {
        const gatedBadge = document.createElement('span');
        gatedBadge.className = 'tag tag-gated';
        gatedBadge.textContent = 'Gated';
        badges.appendChild(gatedBadge);
      }

      const hfLink = document.createElement('a');
      hfLink.href = `https://huggingface.co/${result.id}`;
      hfLink.target = '_blank';
      hfLink.rel = 'noreferrer';
      hfLink.className = 'text-link';
      hfLink.textContent = 'View on Hugging Face';
      badges.appendChild(hfLink);

      const downloadBtn = document.createElement('button');
      downloadBtn.type = 'button';
      downloadBtn.className = 'search-download-btn';
      const alreadyInstalled = installedLocalModels.some(
          model => model.repository === result.id);
      if (alreadyInstalled) {
        downloadBtn.textContent = 'Installed';
        downloadBtn.disabled = true;
      } else {
        downloadBtn.textContent = 'Download';
        downloadBtn.addEventListener('click', async () => {
          downloadBtn.disabled = true;
          downloadBtn.textContent = 'Inspecting…';
          modelSearchStatus.textContent = `Checking ${result.id} against Ark's bundled runtime…`;
          try {
            const {state} = await pageHandler.startLocalModelDownload(result.id, true);
            updateLocalModelUI(state);
            if (state.state === 'error') {
              modelSearchStatus.textContent = state.detail;
              downloadBtn.textContent = 'Not compatible';
              return;
            }
            modelSearchStatus.textContent = `${state.displayName} · ${state.variant} · download started in Ark.`;
            downloadBtn.textContent = 'Downloading';
            get('prepared-model-title').scrollIntoView({behavior: 'smooth'});
          } catch (error) {
            console.warn('Failed to inspect model:', error);
            modelSearchStatus.textContent = 'Ark could not inspect this repository right now.';
            downloadBtn.disabled = false;
            downloadBtn.textContent = 'Retry';
          }
        });
      }
      badges.appendChild(downloadBtn);

      card.appendChild(badges);
      modelSearchResults.appendChild(card);
    }
  } catch {
    modelSearchStatus.textContent = 'Search failed. Please check your network connection.';
  }
});

void refreshLocalModelState();
void refreshInstalledModels();

(window as unknown as {__arkTest?: unknown}).__arkTest = {
  renderMessageBubble,
  renderMarkdown,
  highlightCode,
  setSafeHTML,
  loadMessages,
  loadConversations,
  sendMessage,
  safeHtmlPolicy,
  formatMessageTimestamp,
  generateChatTitle,
  createNewChat,
  switchToConversation,
  getGeneratingConversationIds: () => [...generatingConversationIds],
  getCurrentMessages: () => currentMessages,
  getConversationsList: () => conversationsList,
  getActiveModel: () => activeModel,
};
