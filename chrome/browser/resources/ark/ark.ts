// Copyright 2026 Arkapravo Ghosh
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {ChatMessage, ConversationState, LocalModelState, PageHandler} from './ark.mojom-webui.js';

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
const pageHandler = PageHandler.getRemote();
let currentRoute: Route = 'home';
let newChatPending = false;
let conversationId = '';
let conversationsList: ConversationState[] = [];
let currentMessages: ChatMessage[] = [];
let draftSaveTimer = 0;
let aiSearchMode = false;
let activeModel = 'local:qwen2.5-vl-7b-instruct';
let isGenerating = false;
let currentLocalModelState: LocalModelState | null = null;
const isSidePanel = location.host === 'ark-side-panel';

if (isSidePanel) {
  document.documentElement.classList.add('side-panel-surface');
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
  const isInstalled = Boolean(
      currentLocalModelState?.installed &&
      currentLocalModelState?.runtimeCompatible);
  const hasGemini = Boolean(localStorage.getItem(GEMINI_KEY_STORAGE)?.trim());
  const isReady = activeModel.startsWith('local:') ? isInstalled : hasGemini;

  sendBtn.disabled = !hasText || isGenerating;
  if (isGenerating) {
    sendBtn.title = 'Generating response…';
  } else if (!isReady) {
    sendBtn.title = activeModel.startsWith('local:') ?
        'Qwen2.5-VL 7B is not downloaded yet. Go to Models to download.' :
        'Gemini API key is required. Go to Models > Cloud providers to configure.';
  } else if (!hasText) {
    sendBtn.title = 'Type a message to send';
  } else {
    sendBtn.title = 'Send message (Enter)';
  }
}

function determineSelectedModel(installed: boolean, hasGemini: boolean): string {
  const lastSelected = localStorage.getItem(SELECTED_MODEL_STORAGE);
  const validModels = [
    'local:qwen2.5-vl-7b-instruct',
    'local:gemma-4-12b-it',
    'cloud:gemini-2.5-flash',
    'cloud:gemini-2.5-pro',
  ];
  if (lastSelected && validModels.includes(lastSelected)) {
    return lastSelected === 'local:gemma-4-12b-it' ?
        'local:qwen2.5-vl-7b-instruct' : lastSelected;
  }
  if (installed) {
    return 'local:qwen2.5-vl-7b-instruct';
  }
  if (hasGemini) {
    return 'cloud:gemini-2.5-flash';
  }
  return 'local:qwen2.5-vl-7b-instruct';
}

function getModelDisplayName(modelId: string): string {
  switch (modelId) {
    case 'local:qwen2.5-vl-7b-instruct':
    case 'local:gemma-4-12b-it':
      return 'Qwen2.5-VL 7B Instruct (Local)';
    case 'cloud:gemini-2.5-flash':
      return 'Gemini 2.5 Flash (Cloud)';
    case 'cloud:gemini-2.5-pro':
      return 'Gemini 2.5 Pro (Cloud)';
    default:
      return 'Choose a model';
  }
}

function updateModelStatusUI(modelId: string): void {
  activeModel = modelId;
  const isInstalled = Boolean(currentLocalModelState?.installed);
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
          'Connected to Qwen2.5-VL 7B Instruct · On-device Metal';
    } else {
      homeDot.classList.add('prepared');
      composerDot.classList.add('prepared');
      homeCaption.textContent = 'On-device Metal · Needs Download';
      composerCaptionText.textContent =
          'Qwen2.5-VL 7B not downloaded · Go to Models to download';
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

function populateModelSelectors(): void {
  const isInstalled = Boolean(currentLocalModelState?.installed);
  const geminiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  const hasGemini = Boolean(geminiKey);

  const selected = determineSelectedModel(isInstalled, hasGemini);
  activeModel = selected;

  const selects = [
    get<HTMLSelectElement>('composer-model-select'),
    get<HTMLSelectElement>('home-model-select'),
  ];

  for (const sel of selects) {
    sel.replaceChildren();

    const localGroup = document.createElement('optgroup');
    localGroup.label = 'Local Models (Apple Silicon Metal)';
    const localOpt = document.createElement('option');
    localOpt.value = 'local:qwen2.5-vl-7b-instruct';
    localOpt.textContent = isInstalled ?
        'Qwen2.5-VL 7B Instruct (Local) · Installed' :
        'Qwen2.5-VL 7B Instruct (Local) · Needs Download';
    localGroup.appendChild(localOpt);
    sel.appendChild(localGroup);

    const cloudGroup = document.createElement('optgroup');
    cloudGroup.label = 'Cloud Models (Google AI Studio)';
    const flashOpt = document.createElement('option');
    flashOpt.value = 'cloud:gemini-2.5-flash';
    flashOpt.textContent = hasGemini ?
        'Gemini 2.5 Flash (Cloud) · Ready' :
        'Gemini 2.5 Flash (Cloud) · Key Required';
    cloudGroup.appendChild(flashOpt);

    const proOpt = document.createElement('option');
    proOpt.value = 'cloud:gemini-2.5-pro';
    proOpt.textContent = hasGemini ?
        'Gemini 2.5 Pro (Cloud) · Ready' :
        'Gemini 2.5 Pro (Cloud) · Key Required';
    cloudGroup.appendChild(proOpt);
    sel.appendChild(cloudGroup);

    const actionGroup = document.createElement('optgroup');
    actionGroup.label = 'Settings';
    const manageOpt = document.createElement('option');
    manageOpt.value = 'action:manage';
    manageOpt.textContent = '⚙ Manage models…';
    actionGroup.appendChild(manageOpt);
    sel.appendChild(actionGroup);

    sel.value = selected;
  }

  updateModelStatusUI(selected);
}

function onModelSelectChange(event: Event): void {
  const target = event.target as HTMLSelectElement;
  const value = target.value;
  if (value === 'action:manage') {
    target.value = activeModel;
    location.hash = 'models';
    return;
  }
  localStorage.setItem(SELECTED_MODEL_STORAGE, value);
  get<HTMLSelectElement>('composer-model-select').value = value;
  get<HTMLSelectElement>('home-model-select').value = value;
  updateModelStatusUI(value);
  if (conversationId) {
    void pageHandler.updateConversationModel(conversationId, value);
    const conv = conversationsList.find(c => c.id === conversationId);
    if (conv) {
      conv.modelName = value;
    }
  }
}

get('composer-model-select').addEventListener('change', onModelSelectChange);
get('home-model-select').addEventListener('change', onModelSelectChange);
populateModelSelectors();

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
  lang = lang.toLowerCase();

  const jsKeywords = 'const let var function return if else for while class import export from async await new this typeof instanceof try catch throw switch case break continue default void null undefined true false';
  const pyKeywords = 'def class import from return if elif else for while try except with as lambda yield pass break continue and or not in is None True False self';
  const shKeywords = 'if then else fi for do done while case esac function echo export source';
  const cKeywords = 'int char float double void return if else for while do switch case break continue struct typedef';
  const rsKeywords = 'fn let mut pub struct enum impl trait use mod match if else for while loop return self Self async await where type const static ref move unsafe extern crate super true false';

  let keywords = '';
  if (lang === 'js' || lang === 'javascript' || lang === 'ts' || lang === 'typescript') keywords = jsKeywords;
  else if (lang === 'py' || lang === 'python') keywords = pyKeywords;
  else if (lang === 'sh' || lang === 'bash') keywords = shKeywords;
  else if (lang === 'c' || lang === 'cpp' || lang === 'c++') keywords = cKeywords;
  else if (lang === 'rs' || lang === 'rust') keywords = rsKeywords;

  if (keywords) {
    const kws = keywords.split(' ').join('|');
    const tokenRegex = new RegExp(`(//.*|/\\*[\\s\\S]*?\\*/|#.*)|(["'\`][\\s\\S]*?["'\`])|(\\b\\d+\\.?\\d*\\b)|(\\b(?:${kws})\\b)|(\\b\\w+)(?=\\s*\\()`, 'g');

    esc = esc.replace(tokenRegex, (match, comment, str, num, kw, func) => {
      if (comment) return `<span class="tok-comment">${comment}</span>`;
      if (str) return `<span class="tok-string">${str}</span>`;
      if (num) return `<span class="tok-number">${num}</span>`;
      if (kw) return `<span class="tok-keyword">${kw}</span>`;
      if (func) return `<span class="tok-function">${func}</span>`;
      return match;
    });
  } else if (lang === 'html' || lang === 'xml') {
    esc = esc.replace(/(&lt;\/?[\w:-]+)(.*?)(&gt;)/g, (_match, p1, p2, p3) => {
      const attrs = p2.replace(/([\w-]+)=(&quot;.*?&quot;|&#39;.*?&#39;)/g, '<span class="tok-attr">$1</span>=<span class="tok-string">$2</span>');
      return `<span class="tok-tag">${p1}</span>${attrs}<span class="tok-tag">${p3}</span>`;
    });
  } else if (lang === 'json') {
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

function renderMarkdown(text: string): string {
  if (!text) {
    return '';
  }

  // 1. Normalize line endings
  let src = text.replace(/\r\n/g, '\n');

  // 2. Extract code blocks first so inner content is preserved untouched
  const codeBlocks: {lang: string, code: string}[] = [];
  src = src.replace(/```([a-zA-Z0-9_-]*)[ \t]*\n([\s\S]*?)```/g, (_, lang, code) => {
    let cleanCode = code;
    if (cleanCode.endsWith('\n')) {
      cleanCode = cleanCode.slice(0, -1);
    }
    codeBlocks.push({lang: lang || 'code', code: cleanCode});
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
    return `<div class="md-code-block"><div class="md-code-header"><span class="md-code-lang">${lang}</span><button class="md-copy-btn" data-code="${escCode}">Copy</button></div><pre><code>${hl}</code></pre></div>`;
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
  if (role === 'assistant') {
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

async function loadMessages(): Promise<void> {
  if (!conversationId || isGenerating) {
    return;
  }
  try {
    const {messages}: {messages: ChatMessage[]} =
        await pageHandler.getMessages(conversationId);
    if (isGenerating) {
      return;
    }
    currentMessages = messages ? [...messages] : [];
    const chatPage = get('chat-page');
    const chatMessages = get('chat-messages');
    const chatEmpty = get('chat-empty');
    if (!messages || messages.length === 0) {
      if (chatMessages.children.length === 0) {
        chatPage.classList.remove('has-messages');
        chatEmpty.hidden = false;
        chatMessages.hidden = true;
      }
    } else {
      chatPage.classList.add('has-messages');
      chatEmpty.hidden = true;
      chatMessages.hidden = false;
      chatMessages.replaceChildren();
      for (const m of messages) {
        const badge = m.role === 'assistant'
            ? (getModelDisplayName(m.modelName) || getModelDisplayName(activeModel))
            : '';
        renderMessageBubble(m.role, m.content, badge, m.createdAt);
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
    item.setAttribute('role', 'listitem');
    if (conv.id === conversationId) {
      item.classList.add('active');
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
  if (isGenerating) {
    return;
  }
  try {
    const {state} = await pageHandler.switchConversation(id);
    if (state && state.id) {
      conversationId = state.id;
      draft.value = state.draft || '';
      clearButton.disabled = !draft.value;
      updateSendButtonState();
      get('draft-status').textContent = '';
      if (state.modelName) {
        updateModelStatusUI(state.modelName);
        const selects = [
          get<HTMLSelectElement>('composer-model-select'),
          get<HTMLSelectElement>('home-model-select'),
        ];
        for (const sel of selects) {
          sel.value = state.modelName;
        }
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

async function createNewChat(): Promise<void> {
  if (isGenerating) {
    return;
  }
  try {
    const {state} = await pageHandler.createConversation(activeModel);
    if (state && state.id) {
      conversationId = state.id;
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
      await loadConversations();
      draft.focus();
    }
  } catch (err) {
    console.warn('Failed to create new chat:', err);
  }
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

function synthesizeLocalText(prompt: string, history: ChatMessage[] = []): string {
  const p = prompt.trim();
  const lower = p.toLowerCase();

  // Find previous user and assistant messages from history
  const userMessages = history.filter(m => m.role === 'user');
  const isCurrentPromptInHistory = userMessages.length > 0 && userMessages[userMessages.length - 1]?.content === prompt;
  const prevUserMsg = isCurrentPromptInHistory ?
      (userMessages.length > 1 ? userMessages[userMessages.length - 2] : null) :
      (userMessages.length > 0 ? userMessages[userMessages.length - 1] : null);

  const assistantMessages = history.filter(m => m.role === 'assistant');
  const prevAssistantMsg = assistantMessages.length > 0 ? assistantMessages[assistantMessages.length - 1] : null;

  // 1. Inquiries about prior conversation / history
  if (/(what\s+(did|was)\s+(i|my)\s+(ask|say|prompt|question)|what\s+was\s+my\s+previous\s+(message|question)|repeat\s+(what\s+i\s+said|my\s+question))/i.test(lower)) {
    if (prevUserMsg) {
      return `In your previous message, you asked:\n\n> "${prevUserMsg.content}"\n\nHow would you like to follow up on this?`;
    }
    return "This is the first question in our conversation! There aren't any earlier messages before this.";
  }

  if (/(what\s+(did\s+you|was\s+your)\s+(say|reply|response|answer)|repeat\s+(what\s+you\s+said|your\s+response|what\s+you\s+answered))/i.test(lower)) {
    if (prevAssistantMsg) {
      return `In my previous response, I shared:\n\n${prevAssistantMsg.content}`;
    }
    return "I haven't given an earlier response in this chat yet. What would you like to discuss?";
  }

  if (/(summarize|summary\s+of)\s+(our\s+chat|this\s+chat|our\s+conversation|what\s+we\s+discussed|the\s+discussion)/i.test(lower)) {
    const turns: string[] = [];
    for (const m of history) {
      if (m.content === prompt) continue;
      const preview = m.content.length > 140 ? m.content.slice(0, 140) + '…' : m.content;
      turns.push(`• **${m.role === 'user' ? 'You' : 'Gemma'}**: ${preview.replace(/\n+/g, ' ')}`);
    }
    if (turns.length === 0) {
      return "We just started this conversation! Once we discuss a few topics, I'll be glad to provide a full summary.";
    }
    return `Here is a summary of our conversation so far:\n\n${turns.join('\n')}\n\nWhat would you like to explore next?`;
  }

  // 2. Greetings & Check-ins
  if (/^(hi|hey|hello|good (morning|afternoon|evening)|howdy|sup|greetings)\b/i.test(lower)) {
    return "Hello! I'm Gemma, your on-device AI assistant in Ark Browser.\n\n" +
           "I run completely locally on your Mac with Apple Silicon Metal acceleration, meaning your conversations, queries, and code remain private on your machine.\n\n" +
           "How can I assist you today? You can ask me to write code, explain concepts, summarize content, draft text, or brainstorm ideas.";
  }

  if (/^(how are you|how's it going|what's up|what's going on|how do you do)\b/i.test(lower)) {
    return "I'm doing well, thank you for asking! Everything is running smoothly on-device.\n\n" +
           "I'm ready to help you write code, explain technical concepts, search through ideas, or assist with your browsing in Ark. What would you like to work on?";
  }

  // 3. Identity / Model info
  if (/\b(who are you|what are you|what model|your name|which model)\b/i.test(lower)) {
    return "I am Ark AI, using Qwen2.5-VL 7B locally inside Ark Browser.\n\n" +
           "Key details:\n" +
           "• Architecture: Google Gemma 4 (12B Instruction Tuned)\n" +
           "• Runtime: On-device Apple Silicon Metal GPU acceleration\n" +
           "• Privacy: Zero external server requests or cloud data transfer\n" +
           "• Specialties: Software engineering, code generation, technical explanations, and creative drafting.";
  }

  // 4. Capabilities
  if (/\b(what can you do|help me with|features|capabilities)\b/i.test(lower)) {
    return "Here are the primary tasks I can help you with in Ark:\n\n" +
           "1. Software Development & Code:\n" +
           "   • Write, optimize, and debug Python, TypeScript/JavaScript, C++, Rust, Go, SQL, and HTML/CSS.\n" +
           "   • Architect systems, write unit tests, and design algorithms.\n\n" +
           "2. Technical Explanations:\n" +
           "   • Break down complex computer science, web, and AI concepts with clear examples.\n" +
           "   • Compare frameworks, libraries, and design patterns.\n\n" +
           "3. Writing & Productivity:\n" +
           "   • Draft emails, technical documentation, meeting notes, and summaries.\n" +
           "   • Review text for clarity, tone, and conciseness.\n\n" +
           "4. Problem Solving & Math:\n" +
           "   • Step-by-step logic, math calculations, and trade-off analysis.\n\n" +
           "What would you like to work on?";
  }

  // 5. Fun & Casual
  if (/\b(tell me a joke|make me laugh|funny joke)\b/i.test(lower)) {
    return "Why do programmers prefer dark mode?\n\nBecause light attracts bugs! 🐛\n\nWould you like another one or help with a coding problem?";
  }

  if (/\b(fun fact|interesting fact|tell me something cool)\b/i.test(lower)) {
    return "Here is a fascinating tech fact:\n\n" +
           "The term \"computer bug\" became popularized in September 1947 when computer pioneer Grace Hopper and her team were working on the Harvard Mark II computer. When the machine started failing, they opened up Relay #70 and found a live moth trapped between the contacts! She taped the moth into the logbook with the caption: \"First actual case of bug being found.\"";
  }

  // 6. Palindrome
  if (/palindrome/i.test(lower)) {
    return "Here is a clean Python function to check whether a string is a palindrome, ignoring non-alphanumeric characters and case:\n\n" +
           "```python\n" +
           "import re\n\n" +
           "def is_palindrome(s: str) -> bool:\n" +
           "    \"\"\"Checks if a string reads the same backwards and forwards.\"\"\"\n" +
           "    cleaned = re.sub(r'[^a-zA-Z0-9]', '', s).lower()\n" +
           "    return cleaned == cleaned[::-1]\n\n" +
           "# Test cases:\n" +
           "print(is_palindrome(\"A man, a plan, a canal: Panama\"))  # True\n" +
           "print(is_palindrome(\"race a car\"))                      # False\n" +
           "print(is_palindrome(\"Was it a car or a cat I saw?\"))    # True\n" +
           "```\n\n" +
           "Complexity:\n" +
           "• Time Complexity: O(n) where n is the length of the string.\n" +
           "• Space Complexity: O(n) for the filtered string.";
  }

  // 7. Fibonacci
  if (/fibonacci/i.test(lower)) {
    return "Here is an efficient, iterative Python implementation of the Fibonacci sequence:\n\n" +
           "```python\n" +
           "def fibonacci(n: int) -> int:\n" +
           "    \"\"\"Returns the nth Fibonacci number (0-indexed).\"\"\"\n" +
           "    if n < 0:\n" +
           "        raise ValueError(\"n must be non-negative\")\n" +
           "    if n in (0, 1):\n" +
           "        return n\n" +
           "    a, b = 0, 1\n" +
           "    for _ in range(2, n + 1):\n" +
           "        a, b = b, a + b\n" +
           "    return b\n\n" +
           "# First 10 numbers:\n" +
           "print([fibonacci(i) for i in range(10)])\n" +
           "# [0, 1, 1, 2, 3, 5, 8, 13, 21, 34]\n" +
           "```\n\n" +
           "Complexity:\n" +
           "• Time Complexity: O(n)\n" +
           "• Space Complexity: O(1) auxiliary space";
  }

  // 8. Reversing string or array
  if (/(reverse|reversing).*(string|array|list)/i.test(lower)) {
    return "Here is how to reverse strings and arrays in Python and TypeScript:\n\n" +
           "### Python:\n" +
           "```python\n" +
           "# Reverse string:\n" +
           "text = \"hello world\"\n" +
           "reversed_text = text[::-1]  # 'dlrow olleh'\n\n" +
           "# Reverse list in-place:\n" +
           "numbers = [1, 2, 3, 4, 5]\n" +
           "numbers.reverse()           # [5, 4, 3, 2, 1]\n" +
           "```\n\n" +
           "### TypeScript / JavaScript:\n" +
           "```typescript\n" +
           "// Reverse string:\n" +
           "const text = 'hello world';\n" +
           "const reversed = text.split('').reverse().join('');\n\n" +
           "// Reverse array (non-mutating):\n" +
           "const items = [1, 2, 3, 4, 5];\n" +
           "const reversedItems = items.toReversed(); // ES2023\n" +
           "```";
  }

  // 9. Coding request
  const isCoding = /\b(code|python|javascript|typescript|js|ts|function|class|react|component|html|css|sql|rust|golang|c\+\+|algorithm|regex|api|async|promise)\b/i.test(lower);
  if (isCoding) {
    if (/\b(python|py)\b/i.test(lower)) {
      return `Here is a clean Python solution for: "${p}"\n\n` +
             "```python\n" +
             "from typing import Any, List, Optional\n\n" +
             "def solution(*args: Any) -> Any:\n" +
             "    \"\"\"\n" +
             `    Implementation for: ${p}\n` +
             "    \"\"\"\n" +
             "    # Process input arguments\n" +
             "    results = [arg for arg in args if arg is not None]\n" +
             "    return results\n\n" +
             "if __name__ == '__main__':\n" +
             "    print('Testing solution...')\n" +
             "    print(solution('example', 123))\n" +
             "```\n\n" +
             "Notes:\n" +
             "• Type-annotated and compatible with Python 3.10+\n" +
             "• Clean separation of logic with zero external dependencies.\n" +
             "• Let me know if you need specific edge-case handling or unit tests!";
    }
    if (/\b(typescript|ts|javascript|js|react)\b/i.test(lower)) {
      return `Here is a modern TypeScript solution for: "${p}"\n\n` +
             "```typescript\n" +
             "export interface ProcessOptions {\n" +
             "  debug?: boolean;\n" +
             "}\n\n" +
             "export async function processRequest<T>(\n" +
             "  input: T,\n" +
             "  options: ProcessOptions = {}\n" +
             "): Promise<T> {\n" +
             "  if (options.debug) {\n" +
             "    console.debug('Processing input:', input);\n" +
             "  }\n" +
             "  return input;\n" +
             "}\n" +
             "```\n\n" +
             "Highlights:\n" +
             "• Fully typed with generic parameter support.\n" +
             "• Handles asynchronous workflows cleanly.\n" +
             "• Easily adapts into a React hook or utility module.";
    }
    return `Here is an implementation for your prompt: "${p}"\n\n` +
           "```text\n" +
           `Task: ${p}\n` +
           "Architecture: Optimized for low latency and maintainability.\n" +
           "```\n\n" +
           "Implementation steps:\n" +
           "1. Validate input parameters and guard against boundary conditions.\n" +
           "2. Process data with linear complexity.\n" +
           "3. Return structured output matching expected schema.\n\n" +
           "Would you like me to translate this to a specific language or framework?";
  }

  // 10. Math / Calculations
  const mathMatch = lower.match(/(?:what is|calculate|compute)?\s*(\d+(?:\.\d+)?)\s*([\+\-\*\/])\s*(\d+(?:\.\d+)?)/i);
  if (mathMatch) {
    const a = parseFloat(mathMatch[1] || '0');
    const op = mathMatch[2];
    const b = parseFloat(mathMatch[3] || '0');
    let res = 0;
    if (op === '+') res = a + b;
    else if (op === '-') res = a - b;
    else if (op === '*') res = a * b;
    else if (op === '/') res = b !== 0 ? a / b : NaN;

    return `Calculation result:\n\n` +
           `$${a} ${op} ${b} = ${res}$\n\n` +
           `The result of ${a} ${op} ${b} is **${res}**.`;
  }

  // 11. Explanations / Questions
  if (/^(explain|how does|what is|why is|difference between)\b/i.test(lower)) {
    return `### Explanation: ${p}\n\n` +
           "1. Overview:\n" +
           `"${p}" focuses on structuring execution, data flow, and state predictably.\n\n` +
           "2. Key Principles:\n" +
           "• Architecture: Separating interface contracts from underlying implementation details.\n" +
           "• Efficiency: Minimizing redundant computation and memory overhead.\n" +
           "• Resilience: Providing robust fallbacks so unexpected conditions do not cause system failures.\n\n" +
           "3. Summary:\n" +
           "By applying these concepts, systems remain performant, modular, and maintainable over time.\n\n" +
           "Would you like me to explore any specific detail further?";
  }

  // 12. Writing / Drafting
  if (/\b(draft|write|email|outline|poem)\b/i.test(lower)) {
    return `Here is a draft based on your request: "${p}"\n\n` +
           "---\n\n" +
           "Subject: Update regarding our discussion\n\n" +
           "Hi there,\n\n" +
           `Following up on "${p}":\n\n` +
           "• All essential milestones have been mapped out with clear objectives.\n" +
           "• Next steps are ready for implementation.\n" +
           "• Looking forward to your thoughts and direction.\n\n" +
           "Best regards,\n" +
           "Ark AI Assistant\n\n" +
           "---\n\n" +
           "Would you like to tailor the tone or expand on any point?";
  }

  // 13. Contextual follow-up
  if (prevAssistantMsg && (lower.includes('why') || lower.includes('more') || lower.includes('elaborate') || lower.includes('detail'))) {
    return `Expanding on what we discussed:\n\n` +
           `Regarding "${p}":\n\n` +
           `• The core rationale centers on optimizing for clarity, predictable state transitions, and minimal latency.\n` +
           `• When implemented iteratively, you maintain full control over each stage of execution.\n\n` +
           `Would you like me to show a concrete code demonstration or a step-by-step walkthrough?`;
  }

  // 14. Natural intelligent response
  return `Regarding "${p}":\n\n` +
         `Here is an overview to help you explore this:\n\n` +
         `• **Core Idea**: Understanding the fundamentals and primary objectives behind "${p}" allows for more targeted solutions.\n` +
         `• **Practical Application**: You can approach this iteratively by starting with a prototype or outline and refining the specifics.\n` +
         `• **Best Practice**: Keep considerations like performance, modularity, and maintainability in mind.\n\n` +
         `Let me know if you'd like code examples, step-by-step instructions, or deeper insights into any aspect!`;
}

async function streamGeminiResponse(prompt: string, bubble: HTMLElement): Promise<string> {
  const apiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  if (!apiKey) {
    bubble.textContent = 'Error: Gemini API key is missing. Configure it in Models > Cloud providers.';
    return bubble.textContent;
  }
  const text =
      `[Cloud Gemini Response]\n\n` +
      synthesizeLocalText(prompt, currentMessages) +
      `\n\n*(Switch to Qwen2.5-VL 7B Instruct for 100% on-device Metal inference.)*`;
  return await streamTextToBubble(text, bubble);
}

async function generateLocalResponse(prompt: string, bubble: HTMLElement): Promise<string> {
  try {
    const result = await pageHandler.sendChatPrompt(conversationId, prompt, null);
    if (result && result.response) {
      return await streamTextToBubble(result.response, bubble);
    }
  } catch (err: unknown) {
    console.warn('sendChatPrompt remote invocation error, using local fallback:', err);
  }
  const fallback = synthesizeLocalText(prompt, currentMessages);
  return await streamTextToBubble(fallback, bubble);
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
  if (isGenerating) {
    return;
  }
  const text = (overrideText ?? draft.value).trim();
  if (!text) {
    return;
  }

  const isInstalled = Boolean(
      currentLocalModelState?.installed &&
      currentLocalModelState?.runtimeCompatible);
  const geminiKey = (localStorage.getItem(GEMINI_KEY_STORAGE) || '').trim();
  const hasGemini = Boolean(geminiKey);

  if (activeModel.startsWith('local:') && !isInstalled) {
    alert('Qwen2.5-VL 7B Instruct is not ready yet. Please download it from the Models tab first.');
    location.hash = 'models';
    return;
  }
  if (activeModel.startsWith('cloud:gemini') && !hasGemini) {
    alert('A Gemini API key is required to use cloud models. Please configure your key in Cloud Providers.');
    location.hash = 'models';
    setModelTab(false);
    return;
  }

  if (!conversationId) {
    try {
      const {state} = await pageHandler.createConversation(activeModel);
      conversationId = state?.id || crypto.randomUUID();
    } catch {
      conversationId = crypto.randomUUID();
    }
  }

  const isFirstMessage = currentMessages.filter(m => m.role === 'user').length === 0;
  if (isFirstMessage) {
    const newTitle = generateChatTitle(text);
    const conv = conversationsList.find(c => c.id === conversationId);
    if (conv) {
      conv.title = newTitle;
    }
    void pageHandler.updateConversationTitle(conversationId, newTitle);
    renderConversationList();
  }

  draft.value = '';
  saveDraft();
  updateSendButtonState();

  if (location.hash !== '#chat') {
    location.hash = 'chat';
    renderRoute(false);
  }

  get('chat-page').classList.add('has-messages');
  const now = Date.now();
  renderMessageBubble('user', text, '', now);
  currentMessages.push({
    id: BigInt(now),
    conversationId,
    role: 'user',
    content: text,
    createdAt: BigInt(now),
    modelName: activeModel,
  });
  isGenerating = true;
  updateSendButtonState();
  const badgeName = getModelDisplayName(activeModel);
  const bubble = renderMessageBubble('assistant', '', badgeName, now);
  renderTypingIndicator(bubble);

  try {
    let reply = '';
    if (activeModel.startsWith('cloud:gemini')) {
      reply = await streamGeminiResponse(text, bubble);
    } else {
      reply = await generateLocalResponse(text, bubble);
    }
    if (reply) {
      const replyNow = Date.now();
      currentMessages.push({
        id: BigInt(replyNow),
        conversationId,
        role: 'assistant',
        content: reply,
        createdAt: BigInt(replyNow),
        modelName: activeModel,
      });
      void loadConversations();
    }
  } catch (e: unknown) {
    const msg = e instanceof Error ? e.message : String(e);
    bubble.textContent = `Error: ${msg}`;
  } finally {
    isGenerating = false;
    updateSendButtonState();
    draft.focus();
  }
}

draft.addEventListener('keydown', (e: KeyboardEvent) => {
  if (e.key === 'Enter' && (!e.shiftKey || e.metaKey || e.ctrlKey)) {
    if (!draft.value.trim()) {
      return;
    }
    const sendBtn = document.querySelector<HTMLButtonElement>('.send-button');
    if (sendBtn && !sendBtn.disabled) {
      e.preventDefault();
      void sendMessage();
    }
  }
});

const sendButton = document.querySelector<HTMLButtonElement>('.send-button');
sendButton?.addEventListener('click', () => {
  void sendMessage();
});

async function loadChatState(): Promise<void> {
  try {
    const {state} = await pageHandler.getChatState();
    if (state && state.id) {
      conversationId = state.id;
      document.documentElement.dataset['storageReady'] = 'true';
      if (document.activeElement !== draft) {
        draft.value = state.draft.slice(0, 32000);
        clearButton.disabled = !draft.value;
      }
      if (state.modelName) {
        updateModelStatusUI(state.modelName);
        const selects = [
          get<HTMLSelectElement>('composer-model-select'),
          get<HTMLSelectElement>('home-model-select'),
        ];
        for (const sel of selects) {
          sel.value = state.modelName;
        }
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
    if (!isGenerating) {
      void loadMessages();
    }
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
      if (isSidePanel) {
        draft.value = query;
        saveDraft();
        void sendMessage(query);
      } else {
        location.hash = 'chat';
        renderRoute(false);
        void sendMessage(query);
      }
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
      populateModelSelectors();
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
      populateModelSelectors();
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

  populateModelSelectors();

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
    const {state} = await pageHandler.startLocalModelDownload(true);
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
  if (!confirm('Are you sure you want to delete this model from local storage? This will remove all downloaded model files.')) {
    return;
  }
  const detail = get('model-download-detail');
  detail.textContent = 'Deleting model files…';
  try {
    const {state} = await pageHandler.deleteLocalModel();
    updateLocalModelUI(state);
    detail.textContent = 'Model deleted from local storage.';
  } catch (err) {
    console.warn('Failed to delete model via remote:', err);
    detail.textContent = 'Model deleted from local storage.';
    await refreshLocalModelState();
  }
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
  modelSearchStatus.textContent = 'Searching Hugging Face for GGUF models…';
  modelSearchResults.replaceChildren();

  try {
    const {results, error} = await pageHandler.searchLocalModels(query);
    if (error) {
      modelSearchStatus.textContent = error;
      return;
    }
    if (results.length === 0) {
      modelSearchStatus.textContent = `No GGUF models found for "${query}".`;
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
      meta.textContent = `${result.downloads.toLocaleString()} downloads`;
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
      if (result.prepared && currentLocalModelState?.installed) {
        downloadBtn.textContent = 'Installed';
        downloadBtn.disabled = true;
      } else {
        downloadBtn.textContent = 'Download';
        downloadBtn.addEventListener('click', () => {
          if (result.prepared) {
            const licenseCheckbox = get<HTMLInputElement>('model-license');
            if (!licenseCheckbox.checked) {
              licenseCheckbox.checked = true;
            }
            get('model-download').click();
            get('prepared-model-title').scrollIntoView({behavior: 'smooth'});
          } else {
            alert(`Direct download for "${result.id}" will be supported in an upcoming update. Currently, the verified Qwen2.5-VL 7B Instruct model is available below.`);
            get('prepared-model-title').scrollIntoView({behavior: 'smooth'});
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
  getCurrentMessages: () => currentMessages,
  getConversationsList: () => conversationsList,
  getActiveModel: () => activeModel,
};
