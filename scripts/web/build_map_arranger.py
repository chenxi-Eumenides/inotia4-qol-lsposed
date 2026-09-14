import base64
import json
from io import BytesIO
from pathlib import Path

from PIL import Image


PROJECT_ROOT = Path(__file__).resolve().parents[2]
MAP_INFO_PATH = PROJECT_ROOT / "apk/static-data/json/tables/MAPINFOBASE.json"
TILES_PATH = PROJECT_ROOT / "apk/static-data/json/maps/tiles.json"
EXITS_PATH = PROJECT_ROOT / "apk/static-data/json/maps/exits.json"
OUTPUT_PATH = PROJECT_ROOT / "web/map-arranger.html"


HTML_TEMPLATE = r'''<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="color-scheme" content="light dark">
  <title>地图摆放工具 · Inotia 4</title>
  <style>
    :root {
      color-scheme: light;
      --ink: #18212b;
      --muted: #65717d;
      --soft: #9aa4ad;
      --panel: #fffdf8;
      --panel-strong: #ffffff;
      --stage: #eef1f3;
      --stage-grid: rgba(24, 33, 43, .055);
      --border: #d8dde1;
      --border-strong: #bbc4ca;
      --accent: #e4572e;
      --accent-soft: rgba(228, 87, 46, .12);
      --link: #0f766e;
      --link-soft: rgba(13, 148, 136, .13);
      --connection: #b53d25;
      --connection-soft: rgba(181, 61, 37, .12);
      --success: #16a34a;
      --shadow: 0 16px 45px rgba(20, 31, 41, .12);
      --card-shadow: 0 9px 18px rgba(20, 31, 41, .2), 0 2px 4px rgba(20, 31, 41, .12);
      --header-height: 76px;
      --sidebar-width: 292px;
      --cell: 5px;
    }

    body[data-theme="dark"] {
      color-scheme: dark;
      --ink: #e9eef2;
      --muted: #a8b2bb;
      --soft: #73808b;
      --panel: #151b20;
      --panel-strong: #1d252c;
      --stage: #080b0e;
      --stage-grid: rgba(232, 239, 244, .055);
      --border: #2e3840;
      --border-strong: #45515b;
      --accent: #ff7652;
      --accent-soft: rgba(255, 118, 82, .14);
      --link: #5eead4;
      --link-soft: rgba(45, 212, 191, .16);
      --connection: #ff896f;
      --connection-soft: rgba(255, 137, 111, .16);
      --shadow: 0 16px 45px rgba(0, 0, 0, .35);
      --card-shadow: 0 12px 24px rgba(0, 0, 0, .52), 0 2px 5px rgba(0, 0, 0, .45);
    }

    * { box-sizing: border-box; }

    html, body { height: 100%; }

    body {
      margin: 0;
      overflow: hidden;
      color: var(--ink);
      background: var(--stage);
      font-family: "Noto Sans CJK SC", "PingFang SC", "Microsoft YaHei", system-ui, sans-serif;
      transition: background-color .2s ease, color .2s ease;
    }

    button, input { font: inherit; }
    button { cursor: pointer; }

    .sr-only {
      position: absolute;
      width: 1px;
      height: 1px;
      padding: 0;
      margin: -1px;
      overflow: hidden;
      clip: rect(0, 0, 0, 0);
      white-space: nowrap;
      border: 0;
    }

    .app {
      display: grid;
      grid-template-rows: var(--header-height) 1fr;
      height: 100%;
    }

    .topbar {
      position: relative;
      z-index: 30;
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 20px;
      min-width: 0;
      padding: 0 24px 0 28px;
      border-bottom: 1px solid var(--border);
      background: color-mix(in srgb, var(--panel) 92%, transparent);
      box-shadow: 0 5px 20px rgba(15, 23, 32, .06);
      backdrop-filter: blur(14px);
    }

    .brand {
      display: flex;
      align-items: center;
      gap: 14px;
      min-width: 0;
    }

    .brand-mark {
      position: relative;
      display: grid;
      place-items: center;
      width: 34px;
      height: 34px;
      flex: 0 0 34px;
      border: 1px solid var(--accent);
      border-radius: 10px 3px 10px 3px;
      color: var(--accent);
      background: var(--accent-soft);
    }

    .brand-mark::after {
      content: "";
      width: 10px;
      height: 10px;
      border: 1px solid currentColor;
      border-radius: 50%;
      box-shadow: 0 0 0 3px color-mix(in srgb, currentColor 22%, transparent);
    }

    .brand-copy { min-width: 0; }

    .eyebrow {
      margin-bottom: 2px;
      color: var(--accent);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
      font-weight: 700;
      letter-spacing: .16em;
      text-transform: uppercase;
    }

    h1 {
      margin: 0;
      overflow: hidden;
      font-family: "STKaiti", "KaiTi", "Noto Serif CJK SC", serif;
      font-size: clamp(20px, 2.1vw, 27px);
      font-weight: 700;
      letter-spacing: .04em;
      line-height: 1.05;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .top-actions {
      display: flex;
      align-items: center;
      gap: 8px;
      flex: 0 0 auto;
    }

    .action-button {
      display: inline-flex;
      align-items: center;
      justify-content: center;
      gap: 8px;
      min-height: 42px;
      padding: 0 13px;
      border: 1px solid var(--border);
      border-radius: 9px;
      color: var(--ink);
      background: var(--panel-strong);
      font-size: 13px;
      font-weight: 700;
      transition: border-color .2s ease, color .2s ease, background-color .2s ease, transform .2s ease;
    }

    .action-button:hover {
      border-color: var(--accent);
      color: var(--accent);
      background: var(--accent-soft);
    }

    .action-button:active { transform: translateY(1px); }

    .action-button:focus-visible,
    .map-item:focus-visible,
    .search-input:focus-visible,
    .dialog-button:focus-visible {
      outline: 3px solid color-mix(in srgb, var(--accent) 38%, transparent);
      outline-offset: 2px;
    }

    .action-button svg { width: 17px; height: 17px; stroke: currentColor; }

    .icon-only {
      width: 42px;
      padding: 0;
    }

    .icon-only .moon-icon { display: none; }
    body[data-theme="dark"] .icon-only .sun-icon { display: none; }
    body[data-theme="dark"] .icon-only .moon-icon { display: block; }

    .main-grid {
      display: grid;
      grid-template-columns: var(--sidebar-width) minmax(0, 1fr);
      min-height: 0;
    }

    .library {
      position: relative;
      z-index: 20;
      display: flex;
      flex-direction: column;
      min-height: 0;
      border-right: 1px solid var(--border);
      background: var(--panel);
      transition: background-color .2s ease, border-color .2s ease;
    }

    .library.is-drop-target {
      border-right-color: var(--accent);
      box-shadow: inset -4px 0 0 var(--accent);
    }

    .library-head {
      padding: 22px 18px 14px;
      border-bottom: 1px solid var(--border);
    }

    .section-kicker {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 12px;
      margin-bottom: 13px;
    }

    .section-kicker h2 {
      margin: 0;
      font-size: 13px;
      letter-spacing: .08em;
    }

    .count-badge {
      min-width: 32px;
      padding: 3px 7px;
      border: 1px solid color-mix(in srgb, var(--accent) 45%, var(--border));
      border-radius: 999px;
      color: var(--accent);
      background: var(--accent-soft);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 11px;
      text-align: center;
    }

    .search-wrap { position: relative; }

    .search-wrap svg {
      position: absolute;
      top: 50%;
      left: 12px;
      width: 16px;
      height: 16px;
      color: var(--muted);
      pointer-events: none;
      transform: translateY(-50%);
    }

    .search-input {
      width: 100%;
      min-height: 42px;
      padding: 0 12px 0 37px;
      border: 1px solid var(--border);
      border-radius: 9px;
      color: var(--ink);
      outline: none;
      background: var(--stage);
      transition: border-color .2s ease, box-shadow .2s ease, background-color .2s ease;
    }

    .search-input::placeholder { color: var(--soft); }
    .search-input:focus { border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-soft); }

    .library-note {
      margin: 11px 2px 0;
      color: var(--muted);
      font-size: 11px;
      line-height: 1.55;
    }

    .map-list {
      min-height: 0;
      padding: 10px;
      overflow-y: auto;
      scrollbar-color: var(--border-strong) transparent;
    }

    .map-item {
      display: flex;
      align-items: center;
      gap: 10px;
      width: 100%;
      min-height: 48px;
      margin: 2px 0;
      padding: 7px 9px;
      border: 1px solid transparent;
      border-radius: 8px;
      color: var(--ink);
      text-align: left;
      background: transparent;
      touch-action: none;
      transition: border-color .16s ease, background-color .16s ease, color .16s ease;
    }

    .map-item:hover {
      border-color: var(--border);
      background: var(--stage);
    }

    .map-item.is-linked {
      border-color: var(--link);
      background: var(--link-soft);
      box-shadow: inset 3px 0 0 var(--link);
    }

    .map-item.is-linked .map-item-index {
      color: var(--link);
      background: color-mix(in srgb, var(--link) 17%, transparent);
    }

    .map-item.is-dragging { opacity: .42; }
    .map-item.is-isolated { opacity: .55; }
    .map-item[hidden], .empty-list[hidden] { display: none; }

    .map-item-index {
      display: grid;
      place-items: center;
      width: 27px;
      height: 27px;
      flex: 0 0 27px;
      border-radius: 6px 2px 6px 2px;
      color: var(--accent);
      background: var(--accent-soft);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
      font-weight: 700;
    }

    .map-item-copy { min-width: 0; }

    .map-item-name {
      display: block;
      overflow: hidden;
      font-size: 13px;
      line-height: 1.35;
      text-overflow: ellipsis;
      white-space: nowrap;
    }

    .map-item-id {
      display: block;
      margin-top: 2px;
      color: var(--muted);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
    }

    .empty-list {
      padding: 24px 12px;
      color: var(--muted);
      font-size: 12px;
      line-height: 1.7;
      text-align: center;
    }

    .workspace {
      display: grid;
      grid-template-rows: 58px minmax(0, 1fr) 34px;
      min-width: 0;
      min-height: 0;
      background: var(--stage);
    }

    .workspace-toolbar {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 14px;
      min-width: 0;
      padding: 0 20px;
      border-bottom: 1px solid var(--border);
      background: color-mix(in srgb, var(--panel) 72%, transparent);
    }

    .workspace-title {
      overflow: hidden;
      color: var(--muted);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
      font-weight: 700;
      letter-spacing: .14em;
      text-overflow: ellipsis;
      text-transform: uppercase;
      white-space: nowrap;
    }

    .legend {
      display: flex;
      align-items: center;
      gap: 13px;
      color: var(--muted);
      font-size: 11px;
      white-space: nowrap;
    }

    .legend-item { display: inline-flex; align-items: center; gap: 5px; }

    .legend-swatch {
      width: 9px;
      height: 9px;
      border-radius: 2px;
      border: 1px solid rgba(0, 0, 0, .16);
    }

    .legend-exit { background: #16a34a; }
    .legend-block { background: #1e293b; }
    .legend-ground { background: #d9dce2; }

    .stage {
      position: relative;
      min-width: 0;
      min-height: 0;
      overflow: hidden;
      cursor: grab;
      isolation: isolate;
      touch-action: none;
      background-color: var(--stage);
      background-image:
        linear-gradient(var(--stage-grid) 1px, transparent 1px),
        linear-gradient(90deg, var(--stage-grid) 1px, transparent 1px);
      background-position: 0 0, 0 0;
      background-size: 28px 28px;
    }

    .stage:active { cursor: grabbing; }
    .stage.is-drop-target { box-shadow: inset 0 0 0 2px var(--accent); }

    .world {
      position: absolute;
      inset: 0;
      transform-origin: 0 0;
      will-change: transform;
    }

    .connections {
      position: absolute;
      inset: 0;
      z-index: 1;
      width: 100%;
      height: 100%;
      overflow: visible;
      pointer-events: none;
    }

    .connection-line {
      position: absolute;
      top: 0;
      left: 0;
      width: 1px;
      height: 1.7px;
      border-radius: 999px;
      background: repeating-linear-gradient(
        90deg,
        var(--connection) 0 5px,
        transparent 5px 9px
      );
      opacity: .82;
      box-shadow: 0 0 0 1.4px color-mix(in srgb, var(--stage) 90%, transparent);
      transform-origin: left center;
    }

    .cards { position: absolute; inset: 0; z-index: 2; pointer-events: none; }

    .map-card {
      position: absolute;
      display: block;
      min-width: 5px;
      min-height: 5px;
      border: 0;
      border-radius: 5px;
      cursor: grab;
      overflow: hidden;
      pointer-events: auto;
      user-select: none;
      box-shadow: var(--card-shadow);
      outline: 2px solid rgba(255, 255, 255, .9);
      transition: outline-color .16s ease, box-shadow .16s ease, opacity .16s ease;
    }

    .map-card:hover { outline-color: var(--accent); box-shadow: 0 12px 25px rgba(20, 31, 41, .28), 0 0 0 3px var(--accent-soft); }
    .map-card.is-selected {
      outline: 3px solid var(--accent);
      outline-offset: 1px;
      box-shadow: 0 0 0 4px var(--accent-soft), var(--card-shadow);
    }
    .map-card:focus-visible { outline: 3px solid color-mix(in srgb, var(--accent) 52%, transparent); outline-offset: 3px; }
    .map-card.is-dragging {
      z-index: 10;
      cursor: grabbing;
      opacity: .88;
      will-change: transform;
    }

    .map-canvas {
      display: block;
      width: 100%;
      height: 100%;
      image-rendering: pixelated;
      image-rendering: crisp-edges;
    }

    .map-label {
      position: absolute;
      top: 50%;
      left: 50%;
      max-width: calc(100% - 12px);
      padding: 5px 8px 4px;
      overflow: hidden;
      border: 1px solid rgba(255, 255, 255, .48);
      border-radius: 5px;
      color: #fff;
      background: rgba(20, 28, 35, .73);
      box-shadow: 0 3px 9px rgba(0, 0, 0, .24);
      font-size: 11px;
      font-weight: 700;
      line-height: 1.25;
      text-align: center;
      text-overflow: ellipsis;
      text-shadow: 0 1px 2px rgba(0, 0, 0, .65);
      transform: translate(-50%, -50%);
      white-space: nowrap;
      backdrop-filter: blur(2px);
    }

    .map-label small {
      display: block;
      margin-top: 2px;
      color: rgba(255, 255, 255, .73);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 9px;
      font-weight: 500;
    }

    .exit-dot {
      position: absolute;
      z-index: 3;
      width: 9px;
      height: 9px;
      border: 2px solid #ecfdf5;
      border-radius: 50%;
      background: #16a34a;
      box-shadow: 0 0 0 1px rgba(22, 163, 74, .9), 0 2px 7px rgba(0, 0, 0, .45);
      pointer-events: none;
      transform: translate(-50%, -50%);
    }

    .stage-empty {
      position: absolute;
      top: 50%;
      left: 50%;
      z-index: 4;
      width: min(360px, calc(100% - 40px));
      padding: 20px 24px;
      border: 1px dashed var(--border-strong);
      border-radius: 12px 4px 12px 4px;
      color: var(--muted);
      background: color-mix(in srgb, var(--panel) 72%, transparent);
      font-family: "STKaiti", "KaiTi", "Noto Serif CJK SC", serif;
      font-size: 16px;
      line-height: 1.7;
      text-align: center;
      pointer-events: none;
      transform: translate(-50%, -50%);
      transition: opacity .2s ease;
    }

    .stage-empty.is-hidden { opacity: 0; }

    .workspace-footer {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 10px;
      padding: 0 20px;
      border-top: 1px solid var(--border);
      color: var(--muted);
      background: var(--panel);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
    }

    .footer-tip { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
    .footer-coords { flex: 0 0 auto; color: var(--soft); }

    .drag-ghost {
      position: fixed;
      z-index: 100;
      max-width: 230px;
      padding: 8px 11px;
      border: 1px solid var(--accent);
      border-radius: 7px;
      color: var(--ink);
      background: var(--panel-strong);
      box-shadow: var(--shadow);
      font-size: 12px;
      font-weight: 700;
      pointer-events: none;
      transform: translate(16px, 16px) rotate(-1deg);
    }

    .drag-ghost small {
      display: block;
      margin-top: 2px;
      color: var(--muted);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 9px;
      font-weight: 500;
    }

    .toast {
      position: fixed;
      right: 22px;
      bottom: 52px;
      z-index: 80;
      max-width: min(420px, calc(100vw - 44px));
      padding: 11px 14px;
      border: 1px solid var(--border-strong);
      border-left: 3px solid var(--accent);
      border-radius: 8px;
      color: var(--ink);
      background: var(--panel-strong);
      box-shadow: var(--shadow);
      font-size: 12px;
      line-height: 1.5;
      opacity: 0;
      pointer-events: none;
      transform: translateY(8px);
      transition: opacity .2s ease, transform .2s ease;
    }

    .toast.is-visible { opacity: 1; transform: translateY(0); }
    .toast.is-error { border-left-color: #dc2626; }

    .file-drop-hint {
      position: fixed;
      inset: 16px;
      z-index: 70;
      display: grid;
      place-items: center;
      border: 2px dashed var(--accent);
      border-radius: 16px 5px 16px 5px;
      color: var(--accent);
      background: color-mix(in srgb, var(--accent-soft) 72%, transparent);
      font-family: "STKaiti", "KaiTi", "Noto Serif CJK SC", serif;
      font-size: clamp(22px, 3vw, 34px);
      font-weight: 700;
      letter-spacing: .04em;
      opacity: 0;
      pointer-events: none;
      transform: scale(.985);
      transition: opacity .16s ease, transform .16s ease;
    }

    body.is-file-drop-target .file-drop-hint {
      opacity: 1;
      transform: scale(1);
    }

    body.is-file-drop-target .stage {
      box-shadow: inset 0 0 0 3px var(--accent);
    }

    dialog {
      width: min(460px, calc(100vw - 32px));
      padding: 0;
      border: 1px solid var(--border-strong);
      border-radius: 13px 4px 13px 4px;
      color: var(--ink);
      background: var(--panel);
      box-shadow: 0 24px 80px rgba(0, 0, 0, .3);
    }

    dialog::backdrop { background: rgba(8, 12, 16, .52); backdrop-filter: blur(3px); }

    .dialog-content { padding: 24px; }

    .dialog-kicker {
      margin-bottom: 7px;
      color: var(--accent);
      font-family: "Fira Code", "SFMono-Regular", Consolas, monospace;
      font-size: 10px;
      font-weight: 700;
      letter-spacing: .15em;
    }

    .dialog-title {
      margin: 0;
      font-family: "STKaiti", "KaiTi", "Noto Serif CJK SC", serif;
      font-size: 24px;
    }

    .dialog-summary {
      margin: 14px 0 20px;
      padding: 13px 14px;
      border: 1px solid var(--border);
      border-radius: 8px;
      color: var(--muted);
      background: var(--stage);
      font-size: 13px;
      line-height: 1.75;
      white-space: pre-line;
    }

    .dialog-actions { display: flex; justify-content: flex-end; gap: 8px; }

    .dialog-button {
      min-height: 42px;
      padding: 0 15px;
      border: 1px solid var(--border);
      border-radius: 8px;
      color: var(--ink);
      background: var(--panel-strong);
      font-size: 13px;
      font-weight: 700;
    }

    .dialog-button.primary { border-color: var(--accent); color: #fff; background: var(--accent); }

    @media (max-width: 760px) {
      body { overflow: auto; }
      .app { min-height: 100%; height: auto; }
      .topbar { min-height: var(--header-height); padding: 0 14px; }
      .top-actions { gap: 5px; }
      .action-button { min-height: 40px; padding: 0 9px; }
      .action-button span { display: none; }
      .icon-only { width: 40px; }
      .main-grid { grid-template-columns: 1fr; min-height: calc(100vh - var(--header-height)); }
      .library { min-height: 260px; max-height: 36vh; border-right: 0; border-bottom: 1px solid var(--border); }
      .library.is-drop-target { border-right: 0; border-bottom-color: var(--accent); box-shadow: inset 0 -4px 0 var(--accent); }
      .library-head { padding: 15px 14px 10px; }
      .map-list { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 2px; }
      .workspace { min-height: 64vh; }
      .legend { display: none; }
      .workspace-toolbar { padding: 0 14px; }
      .workspace-footer { padding: 0 14px; }
    }

    @media (prefers-reduced-motion: reduce) {
      *, *::before, *::after {
        scroll-behavior: auto !important;
        transition-duration: .01ms !important;
        animation-duration: .01ms !important;
        animation-iteration-count: 1 !important;
      }
    }
  </style>
</head>
<body data-theme="light">
  <div class="app">
    <header class="topbar">
      <div class="brand">
        <div class="brand-mark" aria-hidden="true"></div>
        <div class="brand-copy">
          <div class="eyebrow">FIELD ATLAS · OFFLINE</div>
          <h1>游戏地图摆放工具</h1>
        </div>
      </div>
      <div class="top-actions">
        <button class="action-button" id="import-button" type="button" title="导入布局">
          <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 16V3"></path><path d="m7 8 5-5 5 5"></path><path d="M4 14v5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-5"></path></svg>
          <span>导入</span>
        </button>
        <button class="action-button" id="export-button" type="button" title="导出布局">
          <svg viewBox="0 0 24 24" fill="none" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M12 3v13"></path><path d="m17 11-5 5-5-5"></path><path d="M4 14v5a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2v-5"></path></svg>
          <span>导出</span>
        </button>
        <button class="action-button icon-only" id="theme-button" type="button" title="切换到深色主题" aria-label="切换到深色主题">
          <svg class="sun-icon" viewBox="0 0 24 24" fill="none" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><circle cx="12" cy="12" r="4"></circle><path d="M12 2v2M12 20v2M4.93 4.93l1.42 1.42M17.65 17.65l1.42 1.42M2 12h2M20 12h2M4.93 19.07l1.42-1.42M17.65 6.35l1.42-1.42"></path></svg>
          <svg class="moon-icon" viewBox="0 0 24 24" fill="none" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M20.5 15.2A8.5 8.5 0 0 1 8.8 3.5 8.5 8.5 0 1 0 20.5 15.2Z"></path></svg>
        </button>
      </div>
    </header>

    <div class="main-grid">
      <aside class="library" id="library" aria-label="未摆放地图候选栏">
        <div class="library-head">
          <div class="section-kicker">
            <h2>候选地图</h2>
            <span class="count-badge" id="library-count">0</span>
          </div>
          <label class="sr-only" for="map-search">搜索地图名称或 ID</label>
          <div class="search-wrap">
            <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"></circle><path d="m20 20-4-4"></path></svg>
            <input class="search-input" id="map-search" type="search" placeholder="搜索中文名或地图 ID" autocomplete="off">
          </div>
          <p class="library-note">拖出地图到右侧画布；拖回此栏即可收纳。点击条目可放到画布中央。</p>
        </div>
        <div class="map-list" id="map-list"></div>
      </aside>

      <main class="workspace" aria-label="地图摆放画布">
        <div class="workspace-toolbar">
          <div class="workspace-title">MAP ARRANGER / ROUTE STUDY</div>
          <div class="legend" aria-label="瓦片图例">
            <span class="legend-item"><i class="legend-swatch legend-exit"></i>出口</span>
            <span class="legend-item"><i class="legend-swatch legend-block"></i>阻挡</span>
            <span class="legend-item"><i class="legend-swatch legend-ground"></i>地面</span>
          </div>
        </div>
        <div class="stage" id="stage" aria-label="可拖动地图画布">
          <div class="world" id="world">
            <div class="connections" id="connections" aria-hidden="true"></div>
            <div class="cards" id="cards"></div>
          </div>
          <div class="stage-empty" id="stage-empty">从左侧选一张地图开始<br><small>空白处拖动可平移整张地图</small></div>
        </div>
        <div class="workspace-footer">
          <span class="footer-tip" id="layout-status">0 / 416 张地图已摆放</span>
          <span class="footer-coords" id="pan-status">偏移 0, 0</span>
        </div>
      </main>
    </div>
  </div>

  <input id="import-file" type="file" accept="application/json,.json" hidden>
  <dialog id="import-dialog" aria-labelledby="import-dialog-title">
    <div class="dialog-content">
      <div class="dialog-kicker">LAYOUT IMPORT</div>
      <h2 class="dialog-title" id="import-dialog-title">确认导入布局？</h2>
      <div class="dialog-summary" id="import-summary"></div>
      <div class="dialog-actions">
        <button class="dialog-button" id="import-cancel" type="button">取消</button>
        <button class="dialog-button primary" id="import-confirm" type="button">确认重摆</button>
      </div>
    </div>
  </dialog>
  <div class="toast" id="toast" role="status" aria-live="polite"></div>
  <div class="file-drop-hint" id="file-drop-hint" aria-hidden="true">松开导入布局 JSON</div>

  <script id="map-data" type="application/json">/*__MAP_DATA__*/</script>
  <script>
    "use strict";

    const CELL = 5;
    const MIN_ZOOM = 0.25;
    const MAX_ZOOM = 3;
    const CONNECTION_MID_SCREEN_PX = 80;
    const CONNECTION_LONG_SCREEN_PX = 200;
    // 80px 约等于 16 个基础网格，低于 100% 时过滤近距离出口线；
    // 200px 约等于 40 个基础网格，低于 50% 时只保留跨区域长线。
    const MAP_DATA_ELEMENT = document.getElementById("map-data");
    let MAP_DATA;

    try {
      MAP_DATA = JSON.parse(MAP_DATA_ELEMENT.textContent);
    } catch (error) {
      document.body.textContent = "地图数据读取失败：" + (error instanceof Error ? error.message : String(error));
      throw error;
    }

    const mapById = new Map(MAP_DATA.map((map) => [map.id, map]));
    const state = {
      theme: "light",
      offset: { x: 0, y: 0 },
      zoom: 1,
      placed: new Map(),
      selectedMapIds: new Set(),
      drag: null,
      hoveredMapId: null,
      suppressNextClick: false,
      suppressNextSelectionClick: false
    };

    const body = document.body;
    const library = document.getElementById("library");
    const mapList = document.getElementById("map-list");
    const mapSearch = document.getElementById("map-search");
    const stage = document.getElementById("stage");
    const world = document.getElementById("world");
    const cards = document.getElementById("cards");
    const connections = document.getElementById("connections");
    const connectionRecords = new Map();
    const connectionRecordsByMap = new Map();
    const stageEmpty = document.getElementById("stage-empty");
    const libraryCount = document.getElementById("library-count");
    const layoutStatus = document.getElementById("layout-status");
    const panStatus = document.getElementById("pan-status");
    const toast = document.getElementById("toast");
    const importFile = document.getElementById("import-file");
    const importDialog = document.getElementById("import-dialog");
    const importSummary = document.getElementById("import-summary");
    const libraryEntries = new Map();
    let pendingImport = null;
    let toastTimer = null;
    let libraryEmpty = null;
    let searchTimer = null;
    let queuedPointer = null;
    let pointerFrame = null;
    let dragDropRects = null;

    function cacheDragDropRects() {
      dragDropRects = {
        library: library.getBoundingClientRect(),
        stage: stage.getBoundingClientRect()
      };
      return dragDropRects;
    }

    function getDragDropRects() {
      return dragDropRects || cacheDragDropRects();
    }

    function invalidateDragDropRects() {
      dragDropRects = null;
    }

    function errorMessage(error) {
      return error instanceof Error ? error.message : String(error);
    }

    function showToast(message, type) {
      toast.textContent = message;
      toast.classList.toggle("is-error", type === "error");
      toast.classList.add("is-visible");
      if (toastTimer !== null) window.clearTimeout(toastTimer);
      toastTimer = window.setTimeout(() => {
        toast.classList.remove("is-visible");
        toastTimer = null;
      }, 3600);
    }

    function mapLabel(map) {
      return map.name + " · m" + map.id;
    }

    function positionCard(card, mapId) {
      const position = state.placed.get(mapId);
      if (!position) return;
      card.style.left = position.x + "px";
      card.style.top = position.y + "px";
    }

    function positionCardForDrag(card, deltaX, deltaY) {
      card.style.transform = "translate3d(" + deltaX + "px," + deltaY + "px,0)";
    }

    function commitCardDrag(drag) {
      if (drag.kind !== "card") return;
      for (const mapId of drag.mapIds) {
        const card = drag.cardByMapId.get(mapId);
        if (!card) continue;
        positionCard(card, mapId);
        card.style.transform = "";
        card.style.willChange = "";
      }
    }

    function updateCardSelection(card, mapId) {
      const selected = state.selectedMapIds.has(mapId);
      card.classList.toggle("is-selected", selected);
      card.setAttribute("aria-pressed", String(selected));
    }

    function syncCardSelection() {
      for (const card of cards.querySelectorAll(".map-card")) {
        updateCardSelection(card, Number(card.dataset.mapId));
      }
    }

    function cleanSelection() {
      for (const mapId of state.selectedMapIds) {
        if (!state.placed.has(mapId)) state.selectedMapIds.delete(mapId);
      }
    }

    function clearSelection() {
      if (state.selectedMapIds.size === 0) return;
      state.selectedMapIds.clear();
      syncCardSelection();
    }

    function toggleMapSelection(mapId) {
      if (!state.placed.has(mapId)) return;
      if (state.selectedMapIds.has(mapId)) state.selectedMapIds.delete(mapId);
      else state.selectedMapIds.add(mapId);
      const card = cards.querySelector('[data-map-id="' + mapId + '"]');
      if (card) updateCardSelection(card, mapId);
    }

    function createMapCard(map) {
      const card = document.createElement("article");
      card.className = "map-card";
      card.dataset.mapId = String(map.id);
      card.tabIndex = 0;
      card.setAttribute("role", "button");
      card.setAttribute("aria-label", mapLabel(map) + "，可拖动");
      const targets = new Map();
      for (const exit of map.exits) {
        if (!targets.has(exit.targetMapId)) {
          const target = mapById.get(exit.targetMapId);
          targets.set(exit.targetMapId, target ? mapLabel(target) : "未知 · m" + exit.targetMapId);
        }
      }
      const titleLines = [];
      if (targets.size > 0) {
        for (const label of targets.values()) titleLines.push("传送 → " + label);
        titleLines.push("");
      }
      titleLines.push("拖动摆放", "Shift+点击 多选", "Delete 收纳整组", mapLabel(map));
      card.title = titleLines.join("\n");
      card.style.width = Math.max(1, map.w * CELL) + "px";
      card.style.height = Math.max(1, map.h * CELL) + "px";

      const image = document.createElement("img");
      image.className = "map-canvas";
      image.src = map.image;
      image.alt = mapLabel(map) + " 瓦片图";
      image.draggable = false;
      card.appendChild(image);

      const label = document.createElement("div");
      label.className = "map-label";
      label.textContent = map.name;
      const idText = document.createElement("small");
      idText.textContent = "m" + map.id;
      label.appendChild(idText);
      card.appendChild(label);

      for (const exit of map.exits) {
        const dot = document.createElement("span");
        dot.className = "exit-dot";
        dot.style.left = ((exit.x + 0.5) * CELL) + "px";
        dot.style.top = ((exit.y + 0.5) * CELL) + "px";
        const target = mapById.get(exit.targetMapId);
        dot.title = "出口 " + exit.x + "," + exit.y + " → " + (target ? mapLabel(target) : "m" + exit.targetMapId) + " 落点 " + exit.targetX + "," + exit.targetY;
        card.appendChild(dot);
      }

      card.addEventListener("pointerdown", (event) => beginCardDrag(event, map.id));
      card.addEventListener("click", (event) => {
        if (state.suppressNextSelectionClick) {
          state.suppressNextSelectionClick = false;
          return;
        }
        if (!event.shiftKey) return;
        event.preventDefault();
        event.stopPropagation();
        toggleMapSelection(map.id);
      });
      card.addEventListener("pointerenter", () => {
        state.hoveredMapId = map.id;
        updateLinkedHighlights();
      });
      card.addEventListener("pointerleave", () => {
        if (!state.drag || state.drag.kind !== "card" || state.drag.mapId !== map.id) {
          if (state.hoveredMapId === map.id) state.hoveredMapId = null;
          updateLinkedHighlights();
        }
      });
      card.addEventListener("keydown", (event) => {
        if (event.key === "Delete" || event.key === "Backspace") {
          event.preventDefault();
          removeSelectedMapsOrMap(map.id);
        }
      });
      updateCardSelection(card, map.id);
      positionCard(card, map.id);
      return card;
    }

    function renderCards() {
      cleanSelection();
      cards.replaceChildren();
      for (const map of MAP_DATA) {
        if (state.placed.has(map.id)) cards.appendChild(createMapCard(map));
      }
      stageEmpty.classList.toggle("is-hidden", state.placed.size > 0);
      syncCardSelection();
    }

    function updateLinkedHighlights() {
      const sourceId = state.drag && state.drag.kind === "card" ? state.drag.mapId : state.hoveredMapId;
      const sourceMap = sourceId === null ? null : mapById.get(sourceId);
      const linkedIds = sourceMap ? new Set(sourceMap.linkedIds) : new Set();
      for (const entry of libraryEntries.values()) {
        entry.element.classList.toggle("is-linked", linkedIds.has(entry.map.id));
      }
    }

    function createLibraryItem(map) {
      const item = document.createElement("button");
      item.className = "map-item";
      item.type = "button";
      item.dataset.mapId = String(map.id);
      item.setAttribute("aria-label", "摆放 " + mapLabel(map));
      item.addEventListener("pointerdown", (event) => beginCandidateDrag(event, map.id, item));
      item.addEventListener("click", () => {
        if (state.suppressNextClick) {
          state.suppressNextClick = false;
          return;
        }
        placeAtCenter(map.id);
      });

      const index = document.createElement("span");
      index.className = "map-item-index";
      index.textContent = String(map.id).padStart(3, "0");
      item.appendChild(index);

      const copy = document.createElement("span");
      copy.className = "map-item-copy";
      const name = document.createElement("span");
      name.className = "map-item-name";
      name.textContent = map.name;
      const id = document.createElement("span");
      id.className = "map-item-id";
      id.textContent = "m" + map.id + " · " + map.w + "×" + map.h;
      copy.append(name, id);
      item.appendChild(copy);
      return item;
    }

    function initializeLibrary() {
      const isIsolated = (map) => map.exits.length === 0 && !map.referenced;
      const ordered = MAP_DATA.filter((map) => !isIsolated(map)).concat(MAP_DATA.filter(isIsolated));
      const fragment = document.createDocumentFragment();
      for (const map of ordered) {
        const element = createLibraryItem(map);
        if (isIsolated(map)) {
          element.classList.add("is-isolated");
          element.title = "未与其他地图相连";
        }
        libraryEntries.set(map.id, {
          map,
          element,
          searchable: (map.name + " m" + map.id + " " + map.id).toLocaleLowerCase()
        });
        fragment.appendChild(element);
      }
      libraryEmpty = document.createElement("div");
      libraryEmpty.className = "empty-list";
      mapList.append(fragment, libraryEmpty);
    }

    function renderLibrary() {
      const query = mapSearch.value.trim().toLocaleLowerCase();
      let visibleCount = 0;

      for (const entry of libraryEntries.values()) {
        const visible = !state.placed.has(entry.map.id) && (!query || entry.searchable.includes(query));
        entry.element.hidden = !visible;
        if (visible) visibleCount += 1;
      }
      libraryEmpty.hidden = visibleCount !== 0;
      libraryEmpty.textContent = query ? "没有匹配的未摆放地图。" : "所有地图都已摆放。";
      libraryCount.textContent = String(visibleCount);
      updateLinkedHighlights();
    }

    function scheduleLibraryFilter() {
      if (searchTimer !== null) window.clearTimeout(searchTimer);
      searchTimer = window.setTimeout(() => {
        searchTimer = null;
        renderLibrary();
      }, 100);
    }

    function updateWorldTransform() {
      world.style.transform = "translate3d(" + state.offset.x + "px," + state.offset.y + "px,0) scale(" + state.zoom + ")";
      panStatus.textContent = "偏移 " + Math.round(state.offset.x) + ", " + Math.round(state.offset.y) + " · " + Math.round(state.zoom * 100) + "%";
    }

    function exitPoint(position, x, y) {
      return { x: position.x + (x + 0.5) * CELL, y: position.y + (y + 0.5) * CELL };
    }

    function connectionLength(start, end) {
      return Math.hypot(end.x - start.x, end.y - start.y);
    }

    function setConnectionTransform(element, start, end, scale) {
      const deltaX = end.x - start.x;
      const deltaY = end.y - start.y;
      const angle = Math.atan2(deltaY, deltaX);
      element.style.transform = "translate3d(" + start.x + "px," + start.y + "px,0) rotate(" + angle + "rad) scaleX(" + scale + ")";
    }

    function appendConnection(start, end) {
      const line = document.createElement("div");
      line.classList.add("connection-line");
      const length = connectionLength(start, end);
      const baseLength = Math.max(length, 1);
      line.style.width = baseLength + "px";
      setConnectionTransform(line, start, end, 1);
      connections.appendChild(line);
      return { line, baseLength, connectionLength: length };
    }

    function renderConnections() {
      connections.replaceChildren();
      connectionRecords.clear();
      connectionRecordsByMap.clear();
      const pairs = new Map();

      for (const sourceMap of MAP_DATA) {
        for (const targetMapId of sourceMap.linkedIds) {
          const lowId = Math.min(sourceMap.id, targetMapId);
          const highId = Math.max(sourceMap.id, targetMapId);
          const pairKey = lowId + ":" + highId;
          if (pairs.has(pairKey)) continue;
          const lowMap = mapById.get(lowId);
          const highMap = mapById.get(highId);
          if (!lowMap || !highMap) continue;
          const lowExit = lowMap.exits.find((exit) => exit.targetMapId === highId);
          const highExit = highMap.exits.find((exit) => exit.targetMapId === lowId);
          if (!lowExit || !highExit) continue;
          pairs.set(pairKey, { lowId, highId, lowExit, highExit });
        }
      }

      for (const pair of pairs.values()) {
        const lowPosition = state.placed.get(pair.lowId);
        const highPosition = state.placed.get(pair.highId);
        if (!lowPosition || !highPosition) continue;

        // 只有互为出口的地图对才画线；按无序地图对保证双向只出现一条。
        const elements = appendConnection(
          exitPoint(lowPosition, pair.lowExit.x, pair.lowExit.y),
          exitPoint(highPosition, pair.highExit.x, pair.highExit.y)
        );
        const record = {
          lowId: pair.lowId,
          highId: pair.highId,
          lowExit: pair.lowExit,
          highExit: pair.highExit,
          bidirectional: true,
          ...elements
        };
        rememberConnection(pair.lowId + ":" + pair.highId, record);
        updateConnectionVisibility(record);
      }
    }

    function rememberConnection(key, record) {
      connectionRecords.set(key, record);
      for (const mapId of [record.lowId, record.highId]) {
        let records = connectionRecordsByMap.get(mapId);
        if (!records) {
          records = new Set();
          connectionRecordsByMap.set(mapId, records);
        }
        records.add(record);
      }
    }

    function updateConnection(record) {
      const sourcePosition = state.placed.get(record.lowId);
      const targetPosition = state.placed.get(record.highId);
      if (!sourcePosition || !targetPosition) return;

      const start = exitPoint(sourcePosition, record.lowExit.x, record.lowExit.y);
      const end = exitPoint(targetPosition, record.highExit.x, record.highExit.y);
      const length = connectionLength(start, end);
      setConnectionTransform(record.line, start, end, length / record.baseLength);
      record.connectionLength = length;
      updateConnectionVisibility(record);
    }

    function updateConnectionVisibility(record) {
      if (!record) return;
      const screenLength = record.connectionLength * state.zoom;
      const visible = state.zoom >= 0.999
        || (state.zoom >= 0.5 && screenLength >= CONNECTION_MID_SCREEN_PX)
        || (state.zoom < 0.5 && screenLength >= CONNECTION_LONG_SCREEN_PX);
      record.line.hidden = !visible;
      if (!visible) {
        record.line.style.willChange = "";
      } else if (state.drag && state.drag.kind === "card" && state.drag.connectionRecords.has(record)) {
        record.line.style.willChange = "transform";
      }
    }

    function updateAllConnectionVisibility() {
      for (const record of connectionRecords.values()) updateConnectionVisibility(record);
    }

    function connectionRecordsForMaps(mapIds) {
      const records = new Set();
      for (const mapId of mapIds) {
        const mapRecords = connectionRecordsByMap.get(mapId);
        if (!mapRecords) continue;
        for (const record of mapRecords) records.add(record);
      }
      return records;
    }

    function updateConnectionsForMaps(mapIds) {
      for (const record of connectionRecordsForMaps(mapIds)) updateConnection(record);
    }

    function setConnectionWillChange(records, enabled) {
      for (const record of records) {
        record.line.style.willChange = enabled && !record.line.hidden ? "transform" : "";
      }
    }

    function updateStatus() {
      const count = state.placed.size;
      layoutStatus.textContent = count + " / " + MAP_DATA.length + " 张地图已摆放";
      stageEmpty.classList.toggle("is-hidden", count > 0);
    }

    function renderAll() {
      renderLibrary();
      renderCards();
      updateWorldTransform();
      renderConnections();
      updateStatus();
    }

    function stagePositionFromClient(clientX, clientY, map) {
      const rect = stage.getBoundingClientRect();
      return {
        x: (clientX - rect.left - state.offset.x) / state.zoom - (map.w * CELL) / 2,
        y: (clientY - rect.top - state.offset.y) / state.zoom - (map.h * CELL) / 2
      };
    }

    function placeAtCenter(mapId) {
      const map = mapById.get(mapId);
      if (!map) return;
      const rect = stage.getBoundingClientRect();
      placeMap(mapId, {
        x: (rect.width / 2 - state.offset.x) / state.zoom - (map.w * CELL) / 2,
        y: (rect.height / 2 - state.offset.y) / state.zoom - (map.h * CELL) / 2
      });
    }

    function placeMap(mapId, position) {
      if (!mapById.has(mapId)) return;
      state.placed.set(mapId, { x: Math.round(position.x), y: Math.round(position.y) });
      renderAll();
    }

    function removeMap(mapId) {
      removeMaps([mapId]);
    }

    function removeMaps(mapIds) {
      const removedMapIds = [];
      for (const mapId of mapIds) {
        if (!state.placed.delete(mapId)) continue;
        removedMapIds.push(mapId);
        state.selectedMapIds.delete(mapId);
        if (state.hoveredMapId === mapId) state.hoveredMapId = null;
      }
      if (removedMapIds.length === 0) return;
      cleanSelection();
      renderAll();
      if (removedMapIds.length === 1) showToast("已收纳 " + mapLabel(mapById.get(removedMapIds[0])));
      else showToast("已收纳 " + removedMapIds.length + " 张地图");
    }

    function removeSelectedMapsOrMap(mapId) {
      cleanSelection();
      removeMaps(state.selectedMapIds.size > 0 ? Array.from(state.selectedMapIds) : [mapId]);
    }

    function inside(rect, clientX, clientY) {
      return clientX >= rect.left && clientX <= rect.right && clientY >= rect.top && clientY <= rect.bottom;
    }

    function updateDropHint(clientX, clientY) {
      const rects = getDragDropRects();
      const libraryRect = rects.library;
      const stageRect = rects.stage;
      const inLibrary = inside(libraryRect, clientX, clientY);
      const inStage = inside(stageRect, clientX, clientY);
      library.classList.toggle("is-drop-target", state.drag && state.drag.kind === "card" && inLibrary);
      stage.classList.toggle("is-drop-target", state.drag && state.drag.kind === "candidate" && inStage);
    }

    function clearDropHints() {
      library.classList.remove("is-drop-target");
      stage.classList.remove("is-drop-target");
    }

    function makeDragGhost(map) {
      const ghost = document.createElement("div");
      ghost.className = "drag-ghost";
      ghost.textContent = map.name;
      const id = document.createElement("small");
      id.textContent = "m" + map.id + " · 松开以摆放";
      ghost.appendChild(id);
      document.body.appendChild(ghost);
      return ghost;
    }

    function beginCandidateDrag(event, mapId, item) {
      if (event.button !== 0) return;
      const map = mapById.get(mapId);
      if (!map) return;
      event.preventDefault();
      event.stopPropagation();
      state.drag = {
        kind: "candidate",
        mapId,
        startX: event.clientX,
        startY: event.clientY,
        moved: false,
        item,
        ghost: makeDragGhost(map)
      };
      cacheDragDropRects();
      item.classList.add("is-dragging");
      state.drag.ghost.style.left = event.clientX + "px";
      state.drag.ghost.style.top = event.clientY + "px";
    }

    function beginCardDrag(event, mapId) {
      if (event.button !== 0) return;
      const position = state.placed.get(mapId);
      if (!position) return;
      cleanSelection();
      const mapIds = state.selectedMapIds.has(mapId)
        ? Array.from(state.selectedMapIds)
        : [mapId];
      const startPositions = new Map();
      const dragCards = [];
      const dragCardByMapId = new Map();
      for (const selectedMapId of mapIds) {
        const selectedPosition = state.placed.get(selectedMapId);
        if (!selectedPosition) continue;
        startPositions.set(selectedMapId, { x: selectedPosition.x, y: selectedPosition.y });
        const selectedCard = cards.querySelector('[data-map-id="' + selectedMapId + '"]');
        if (selectedCard) {
          dragCards.push(selectedCard);
          dragCardByMapId.set(selectedMapId, selectedCard);
        }
      }
      const dragConnectionRecords = connectionRecordsForMaps(Array.from(startPositions.keys()));
      event.preventDefault();
      event.stopPropagation();
      state.drag = {
        kind: "card",
        mapId,
        mapIds: Array.from(startPositions.keys()),
        startX: event.clientX,
        startY: event.clientY,
        startPositions,
        moved: false,
        shiftKey: event.shiftKey,
        cards: dragCards,
        cardByMapId: dragCardByMapId,
        connectionRecords: dragConnectionRecords
      };
      cacheDragDropRects();
      setConnectionWillChange(dragConnectionRecords, true);
      for (const dragCard of dragCards) {
        dragCard.classList.add("is-dragging");
        dragCard.style.transform = "translate3d(0,0,0)";
        dragCard.style.willChange = "transform";
      }
      updateLinkedHighlights();
    }

    function processPointerMove(event) {
      const drag = state.drag;
      if (!drag) return;
      const deltaX = event.clientX - drag.startX;
      const deltaY = event.clientY - drag.startY;
      if (Math.abs(deltaX) > 3 || Math.abs(deltaY) > 3) drag.moved = true;
      if (!drag.moved) return;

      if (drag.kind === "card") {
        const worldDeltaX = deltaX / state.zoom;
        const worldDeltaY = deltaY / state.zoom;
        for (const mapId of drag.mapIds) {
          const position = state.placed.get(mapId);
          const startPosition = drag.startPositions.get(mapId);
          if (!position || !startPosition) continue;
          position.y = startPosition.y + worldDeltaY;
          position.x = startPosition.x + worldDeltaX;
          const card = drag.cardByMapId.get(mapId);
          if (card) positionCardForDrag(card, worldDeltaX, worldDeltaY);
        }
        updateConnectionsForMaps(drag.mapIds);
      } else {
        drag.ghost.style.left = event.clientX + "px";
        drag.ghost.style.top = event.clientY + "px";
      }
      updateDropHint(event.clientX, event.clientY);
    }

    function processPanMove(event) {
      const drag = state.drag;
      if (!drag || drag.kind !== "pan") return;
      const deltaX = event.clientX - drag.startX;
      const deltaY = event.clientY - drag.startY;
      if (Math.abs(deltaX) > 2 || Math.abs(deltaY) > 2) drag.moved = true;
      if (!drag.moved) return;
      state.offset.x = drag.startOffset.x + deltaX;
      state.offset.y = drag.startOffset.y + deltaY;
      updateWorldTransform();
    }

    function applyQueuedPointer() {
      const point = queuedPointer;
      queuedPointer = null;
      if (!point || !state.drag) return;
      if (state.drag.kind === "pan") processPanMove(point);
      else processPointerMove(point);
    }

    function queuePointerMove(event) {
      if (!state.drag) return;
      if (event.cancelable) event.preventDefault();
      queuedPointer = { clientX: event.clientX, clientY: event.clientY };
      if (pointerFrame === null) {
        pointerFrame = window.requestAnimationFrame(() => {
          pointerFrame = null;
          applyQueuedPointer();
        });
      }
    }

    function flushQueuedPointer() {
      if (pointerFrame !== null) {
        window.cancelAnimationFrame(pointerFrame);
        pointerFrame = null;
      }
      applyQueuedPointer();
    }

    function finishDrag(event) {
      flushQueuedPointer();
      if (state.drag) {
        const finalPoint = { clientX: event.clientX, clientY: event.clientY };
        if (state.drag.kind === "pan") processPanMove(finalPoint);
        else processPointerMove(finalPoint);
      }
      const drag = state.drag;
      if (!drag) {
        invalidateDragDropRects();
        return;
      }
      if (drag.kind === "pan") {
        if (!drag.moved && !drag.shiftKey) clearSelection();
        cleanupDrag();
        return;
      }
      const pointX = event.clientX;
      const pointY = event.clientY;
      const rects = getDragDropRects();
      const stageRect = rects.stage;
      const libraryRect = rects.library;
      commitCardDrag(drag);

      if (drag.kind === "candidate") {
        if (drag.moved && inside(stageRect, pointX, pointY)) {
          const map = mapById.get(drag.mapId);
          if (map) placeMap(drag.mapId, stagePositionFromClient(pointX, pointY, map));
        } else if (!drag.moved) {
          placeAtCenter(drag.mapId);
        }
      } else if (drag.moved && inside(libraryRect, pointX, pointY)) {
        removeMaps(drag.mapIds);
      } else {
        updateConnectionsForMaps(drag.mapIds);
      }

      state.suppressNextClick = drag.kind === "candidate";
      state.suppressNextSelectionClick = drag.kind === "card" && drag.moved && drag.shiftKey;
      cleanupDrag();
    }

    function cancelDrag() {
      if (!state.drag) {
        invalidateDragDropRects();
        return;
      }
      if (pointerFrame !== null) {
        window.cancelAnimationFrame(pointerFrame);
        pointerFrame = null;
      }
      queuedPointer = null;
      if (state.drag.kind === "card") {
        commitCardDrag(state.drag);
        updateConnectionsForMaps(state.drag.mapIds);
      }
      cleanupDrag();
    }

    function cleanupDrag() {
      const drag = state.drag;
      if (!drag) return;
      if (drag.item) drag.item.classList.remove("is-dragging");
      if (drag.cards) {
        for (const dragCard of drag.cards) {
          dragCard.classList.remove("is-dragging");
          dragCard.style.transform = "";
          dragCard.style.willChange = "";
        }
      }
      if (drag.ghost) drag.ghost.remove();
      if (drag.connectionRecords) setConnectionWillChange(drag.connectionRecords, false);
      if (pointerFrame !== null) {
        window.cancelAnimationFrame(pointerFrame);
        pointerFrame = null;
      }
      queuedPointer = null;
      clearDropHints();
      invalidateDragDropRects();
      state.drag = null;
      updateLinkedHighlights();
    }

    function beginPan(event) {
      if (event.button !== 0 || event.target.closest(".map-card")) return;
      event.preventDefault();
      event.stopPropagation();
      state.drag = {
        kind: "pan",
        startX: event.clientX,
        startY: event.clientY,
        startOffset: { x: state.offset.x, y: state.offset.y },
        shiftKey: event.shiftKey,
        moved: false
      };
      cacheDragDropRects();
    }

    function clampZoom(value) {
      return Math.min(MAX_ZOOM, Math.max(MIN_ZOOM, value));
    }

    function handleWheel(event) {
      event.preventDefault();
      const rect = stage.getBoundingClientRect();
      const pointerX = event.clientX - rect.left;
      const pointerY = event.clientY - rect.top;
      const worldX = (pointerX - state.offset.x) / state.zoom;
      const worldY = (pointerY - state.offset.y) / state.zoom;
      const delta = event.deltaMode === 1 ? event.deltaY * 16 : event.deltaY;
      const nextZoom = clampZoom(state.zoom * Math.exp(-delta * 0.001));
      if (nextZoom === state.zoom) return;

      state.zoom = nextZoom;
      state.offset.x = pointerX - worldX * state.zoom;
      state.offset.y = pointerY - worldY * state.zoom;
      updateWorldTransform();
      updateAllConnectionVisibility();
    }

    function validateLayout(payload) {
      if (!payload || typeof payload !== "object" || Array.isArray(payload)) throw new Error("JSON 顶层必须是对象。");
      if (payload.version !== 1) throw new Error("不支持的布局版本：" + String(payload.version));
      if (!Array.isArray(payload.placed)) throw new Error("缺少 placed 数组。");

      const offsetSource = payload.canvasOffset === undefined ? {} : payload.canvasOffset;
      if (!offsetSource || typeof offsetSource !== "object" || Array.isArray(offsetSource)) throw new Error("canvasOffset 必须是对象。");
      const offset = {
        x: Number.isFinite(offsetSource.x) ? offsetSource.x : 0,
        y: Number.isFinite(offsetSource.y) ? offsetSource.y : 0
      };
      if (payload.zoom !== undefined && !Number.isFinite(payload.zoom)) throw new Error("zoom 必须是有限数字。");
      const zoom = payload.zoom === undefined ? 1 : clampZoom(payload.zoom);
      const placed = [];
      const seen = new Set();
      let unknownCount = 0;
      let duplicateCount = 0;
      let invalidCount = 0;

      for (const entry of payload.placed) {
        if (!entry || typeof entry !== "object" || Array.isArray(entry) || !Number.isInteger(entry.id) || !Number.isFinite(entry.x) || !Number.isFinite(entry.y)) {
          invalidCount += 1;
          continue;
        }
        if (!mapById.has(entry.id)) {
          unknownCount += 1;
          continue;
        }
        if (seen.has(entry.id)) {
          duplicateCount += 1;
          continue;
        }
        seen.add(entry.id);
        placed.push({ id: entry.id, x: entry.x, y: entry.y });
      }
      return { offset, zoom, placed, unknownCount, duplicateCount, invalidCount };
    }

    function summarizeImport(layout) {
      const lines = [
        "将摆放 " + layout.placed.length + " 张地图",
        "画布偏移：" + Math.round(layout.offset.x) + ", " + Math.round(layout.offset.y),
        "画布缩放：" + Math.round(layout.zoom * 100) + "%",
        "JSON 中未列出的地图将回到候选栏。"
      ];
      const skipped = layout.unknownCount + layout.duplicateCount + layout.invalidCount;
      if (skipped > 0) lines.push("跳过 " + skipped + " 条无法应用的记录（未知、重复或格式错误）。");
      return lines.join("\n");
    }

    function applyLayout(layout) {
      state.offset = { x: layout.offset.x, y: layout.offset.y };
      state.zoom = layout.zoom;
      state.placed = new Map(layout.placed.map((entry) => [entry.id, { x: entry.x, y: entry.y }]));
      cleanSelection();
      renderAll();
      showToast("布局已导入：" + layout.placed.length + " 张地图");
    }

    function isJsonFile(file) {
      const name = typeof file.name === "string" ? file.name.toLowerCase() : "";
      return name.endsWith(".json") || file.type === "application/json";
    }

    async function handleImportFile(file) {
      try {
        if (!isJsonFile(file)) throw new Error("仅支持 .json 布局文件。");
        const layout = validateLayout(JSON.parse(await file.text()));
        pendingImport = layout;
        importSummary.textContent = summarizeImport(layout);
        importDialog.showModal();
      } catch (error) {
        showToast("导入失败：" + errorMessage(error), "error");
      }
    }

    function hasFiles(event) {
      const transfer = event.dataTransfer;
      if (!transfer) return false;
      if (transfer.files && transfer.files.length > 0) return true;
      return transfer.types ? Array.from(transfer.types).includes("Files") : false;
    }

    function clearFileDropFeedback() {
      body.classList.remove("is-file-drop-target");
    }

    function handleWindowDragOver(event) {
      if (!hasFiles(event)) return;
      event.preventDefault();
      if (event.dataTransfer) event.dataTransfer.dropEffect = "copy";
      body.classList.add("is-file-drop-target");
    }

    function handleWindowDrop(event) {
      if (!hasFiles(event)) return;
      event.preventDefault();
      clearFileDropFeedback();
      const file = event.dataTransfer && event.dataTransfer.files[0];
      if (file) handleImportFile(file);
      else showToast("导入失败：没有检测到文件。", "error");
    }

    function exportLayout() {
      const payload = {
        version: 1,
        canvasOffset: { x: Math.round(state.offset.x), y: Math.round(state.offset.y) },
        zoom: Number(state.zoom.toFixed(3)),
        placed: Array.from(state.placed, ([id, position]) => ({
          id,
          x: Math.round(position.x),
          y: Math.round(position.y)
        }))
      };
      const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json;charset=utf-8" });
      const url = URL.createObjectURL(blob);
      const link = document.createElement("a");
      link.href = url;
      link.download = "inotia4-map-layout.json";
      document.body.appendChild(link);
      try {
        link.click();
      } finally {
        link.remove();
        URL.revokeObjectURL(url);
      }
      showToast("布局已导出：" + state.placed.size + " 张地图");
    }

    function toggleTheme() {
      state.theme = state.theme === "light" ? "dark" : "light";
      body.dataset.theme = state.theme;
      const dark = state.theme === "dark";
      const themeButton = document.getElementById("theme-button");
      themeButton.title = dark ? "切换到浅色主题" : "切换到深色主题";
      themeButton.setAttribute("aria-label", themeButton.title);
    }

    document.getElementById("export-button").addEventListener("click", exportLayout);
    document.getElementById("theme-button").addEventListener("click", toggleTheme);
    document.getElementById("import-button").addEventListener("click", () => importFile.click());
    mapSearch.addEventListener("input", scheduleLibraryFilter);
    stage.addEventListener("pointerdown", beginPan);
    stage.addEventListener("wheel", handleWheel, { passive: false });

    importFile.addEventListener("change", () => {
      const file = importFile.files && importFile.files[0];
      importFile.value = "";
      if (!file) return;
      handleImportFile(file);
    });

    window.addEventListener("dragover", handleWindowDragOver, { passive: false });
    window.addEventListener("drop", handleWindowDrop, { passive: false });
    window.addEventListener("dragend", clearFileDropFeedback);
    window.addEventListener("dragleave", (event) => {
      if (event.clientX <= 0 || event.clientY <= 0 || event.clientX >= window.innerWidth || event.clientY >= window.innerHeight) {
        clearFileDropFeedback();
      }
    });

    document.getElementById("import-cancel").addEventListener("click", () => {
      pendingImport = null;
      importDialog.close();
    });

    document.getElementById("import-confirm").addEventListener("click", () => {
      if (pendingImport) applyLayout(pendingImport);
      pendingImport = null;
      importDialog.close();
    });

    importDialog.addEventListener("cancel", () => {
      pendingImport = null;
    });

    window.addEventListener("pointermove", (event) => {
      queuePointerMove(event);
    }, { passive: false });
    window.addEventListener("pointerup", finishDrag);
    window.addEventListener("pointercancel", cancelDrag);
    window.addEventListener("resize", invalidateDragDropRects);
    window.addEventListener("keydown", (event) => {
      if (event.key !== "Escape" || importDialog.open || state.selectedMapIds.size === 0) return;
      event.preventDefault();
      clearSelection();
    });

    initializeLibrary();
    renderAll();
  </script>
</body>
</html>
'''


def load_json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise RuntimeError("无法读取 " + str(path) + "：" + str(error)) from error
    except json.JSONDecodeError as error:
        raise RuntimeError("JSON 格式错误 " + str(path) + "：" + str(error)) from error


def valid_int(value):
    return isinstance(value, int) and not isinstance(value, bool)


def dimension(primary, fallback):
    if valid_int(primary) and 1 <= primary <= 64:
        return primary
    if valid_int(fallback) and 1 <= fallback <= 64:
        return fallback
    return 1


def crop_tiles(encoded, width, height, map_id, warnings):
    raw = b""
    if isinstance(encoded, str) and encoded:
        try:
            raw = base64.b64decode(encoded, validate=True)
        except (TypeError, ValueError) as error:
            warnings.append("m" + str(map_id) + " tiles 解码失败，使用空白矩阵（" + str(error) + "）")
    if len(raw) < 4096:
        raw += b"\x00" * (4096 - len(raw))

    cropped = bytearray()
    for y in range(height):
        start = y * 64
        row = raw[start:start + width]
        cropped.extend(row)
        if len(row) < width:
            cropped.extend(b"\x00" * (width - len(row)))
    return base64.b64encode(bytes(cropped)).decode("ascii")


def trim_tiles(encoded, width, height, protected_cells):
    raw = base64.b64decode(encoded)
    left = 0
    top = 0
    right = width
    bottom = height

    def trimmable_row(row):
        return all(
            raw[row * width + column] != 0
            and (raw[row * width + column] & 0x80) == 0
            and (column, row) not in protected_cells
            for column in range(left, right)
        )

    def trimmable_column(column):
        return all(
            raw[row * width + column] != 0
            and (raw[row * width + column] & 0x80) == 0
            and (column, row) not in protected_cells
            for row in range(top, bottom)
        )

    while bottom - top > 1 and trimmable_row(top):
        top += 1
    while bottom - top > 1 and trimmable_row(bottom - 1):
        bottom -= 1
    while right - left > 1 and trimmable_column(left):
        left += 1
    while right - left > 1 and trimmable_column(right - 1):
        right -= 1

    trimmed = b"".join(raw[row * width + left:row * width + right] for row in range(top, bottom))
    return (
        base64.b64encode(trimmed).decode("ascii"),
        right - left,
        bottom - top,
        left,
        top,
    )


def tile_color(value):
    if (value & 0x80) != 0:
        return 22, 163, 74
    if (value & 0x08) != 0:
        return 30, 41, 59
    if value == 0:
        return 217, 220, 226
    gray = min(200, 56 + value * 8)
    return gray, gray - 8, gray


def render_tiles_png(encoded, width, height):
    raw = base64.b64decode(encoded)
    image = Image.new("RGB", (width, height))
    image.putdata([tile_color(value) for value in raw])
    output = BytesIO()
    image.save(output, format="PNG", optimize=True, compress_level=9)
    return "data:image/png;base64," + base64.b64encode(output.getvalue()).decode("ascii")


def merge_exits(exit_record, map_id):
    grouped = {}
    raw_exits = exit_record.get("exits", []) if isinstance(exit_record, dict) else []
    if not isinstance(raw_exits, list):
        return []

    for exit_item in raw_exits:
        if not isinstance(exit_item, dict):
            continue
        x = exit_item.get("x")
        y = exit_item.get("y")
        target_map_id = exit_item.get("targetMapId")
        target_x = exit_item.get("targetX")
        target_y = exit_item.get("targetY")
        values = (x, y, target_map_id, target_x, target_y)
        if not all(valid_int(value) for value in values):
            continue
        if target_map_id == map_id:
            continue
        grouped.setdefault(target_map_id, []).append((x, y, target_x, target_y))

    merged = []
    for target_map_id, cells in sorted(grouped.items()):
        merged.append({
            "x": int(sum(cell[0] for cell in cells) / len(cells)),
            "y": int(sum(cell[1] for cell in cells) / len(cells)),
            "targetMapId": target_map_id,
            "targetX": int(sum(cell[2] for cell in cells) / len(cells)),
            "targetY": int(sum(cell[3] for cell in cells) / len(cells)),
        })
    return merged


def build_maps(map_info, tiles_by_id, exits_by_id, warnings, trim_stats=None, legacy_tiles=None):
    records = map_info.get("records") if isinstance(map_info, dict) else None
    if not isinstance(records, list):
        raise RuntimeError("MAPINFOBASE.json 缺少 records 数组")
    tile_index = tiles_by_id if isinstance(tiles_by_id, dict) else {}
    exit_index = exits_by_id if isinstance(exits_by_id, dict) else {}

    maps = []
    trim_info_by_id = {}
    for map_id, record in enumerate(records):
        record = record if isinstance(record, dict) else {}
        key = "m" + str(map_id)
        tile_record = tile_index.get(key, {})
        exit_record = exit_index.get(key, {})
        tile_record = tile_record if isinstance(tile_record, dict) else {}
        exit_record = exit_record if isinstance(exit_record, dict) else {}

        width = dimension(tile_record.get("width"), exit_record.get("width"))
        height = dimension(tile_record.get("height"), exit_record.get("height"))
        if key not in tile_index or not isinstance(tile_record.get("tiles"), str) or not tile_record.get("tiles"):
            warnings.append(key + " 缺少 tiles，使用空白矩阵")
        name = record.get("text_0")
        if not isinstance(name, str) or not name:
            name = "未命名地图"
            warnings.append(key + " 缺少 text_0，使用未命名地图")

        merged_exits = merge_exits(exit_record, map_id)
        cropped_tiles = crop_tiles(tile_record.get("tiles"), width, height, map_id, warnings)
        protected_cells = set()
        for exit_item in exit_record.get("exits", []) if isinstance(exit_record, dict) else []:
            if isinstance(exit_item, dict) and valid_int(exit_item.get("x")) and valid_int(exit_item.get("y")):
                protected_cells.add((exit_item["x"], exit_item["y"]))
        trimmed_tiles, trimmed_width, trimmed_height, trim_x, trim_y = trim_tiles(cropped_tiles, width, height, protected_cells)
        image = render_tiles_png(trimmed_tiles, trimmed_width, trimmed_height)
        trim_info = {
            "id": map_id,
            "name": name,
            "before_width": width,
            "before_height": height,
            "before_bytes": width * height,
            "after_width": trimmed_width,
            "after_height": trimmed_height,
            "after_bytes": trimmed_width * trimmed_height,
            "trim_x": trim_x,
            "trim_y": trim_y,
            "old_tiles_base64_bytes": len(trimmed_tiles),
            "new_png_data_uri_bytes": len(image),
        }
        trim_info_by_id[map_id] = trim_info
        if trim_stats is not None:
            trim_stats.append(trim_info)
        if legacy_tiles is not None:
            legacy_tiles[map_id] = trimmed_tiles

        maps.append({
            "id": map_id,
            "name": name,
            "w": trimmed_width,
            "h": trimmed_height,
            "image": image,
            "exits": merged_exits,
        })

    for map_data in maps:
        source_trim = trim_info_by_id[map_data["id"]]
        for exit_item in map_data["exits"]:
            exit_item["x"] = min(max(exit_item["x"] - source_trim["trim_x"], 0), map_data["w"] - 1)
            exit_item["y"] = min(max(exit_item["y"] - source_trim["trim_y"], 0), map_data["h"] - 1)
            target_trim = trim_info_by_id.get(exit_item["targetMapId"])
            if target_trim:
                target_width = target_trim["after_width"]
                target_height = target_trim["after_height"]
                exit_item["targetX"] = min(max(exit_item["targetX"] - target_trim["trim_x"], 0), target_width - 1)
                exit_item["targetY"] = min(max(exit_item["targetY"] - target_trim["trim_y"], 0), target_height - 1)

    outgoing_ids = {
        map_data["id"]: {exit_item["targetMapId"] for exit_item in map_data["exits"]}
        for map_data in maps
    }
    referenced_ids = {target_id for targets in outgoing_ids.values() for target_id in targets}
    for map_data in maps:
        targets = outgoing_ids[map_data["id"]]
        map_data["linkedIds"] = sorted(
            target_id
            for target_id in targets
            if target_id in outgoing_ids and map_data["id"] in outgoing_ids[target_id]
        )
        map_data["referenced"] = map_data["id"] in referenced_ids
    return maps


def print_trim_stats(trim_stats):
    before_bytes = sum(item["before_bytes"] for item in trim_stats)
    after_bytes = sum(item["after_bytes"] for item in trim_stats)
    before_width = sum(item["before_width"] for item in trim_stats)
    before_height = sum(item["before_height"] for item in trim_stats)
    after_width = sum(item["after_width"] for item in trim_stats)
    after_height = sum(item["after_height"] for item in trim_stats)
    count = len(trim_stats)
    if count > 0:
        before_average = (before_width / count, before_height / count)
        after_average = (after_width / count, after_height / count)
    else:
        before_average = (0, 0)
        after_average = (0, 0)

    print("削边统计：总瓦片字节数 " + str(before_bytes) + " → " + str(after_bytes) + "（减少 " + str(before_bytes - after_bytes) + "）")
    print(
        "削边统计：平均尺寸 "
        + "{:.2f}×{:.2f}".format(*before_average)
        + " → "
        + "{:.2f}×{:.2f}".format(*after_average)
    )
    print("削减最多的地图：")
    top_trimmed = sorted(
        trim_stats,
        key=lambda item: (-(item["before_bytes"] - item["after_bytes"]), item["id"]),
    )[:3]
    if not top_trimmed:
        print("  无地图数据")
        return
    for item in top_trimmed:
        reduced = item["before_bytes"] - item["after_bytes"]
        print(
            "  m"
            + str(item["id"])
            + " "
            + item["name"]
            + "："
            + str(item["before_width"])
            + "×"
            + str(item["before_height"])
            + " → "
            + str(item["after_width"])
            + "×"
            + str(item["after_height"])
            + "，减少 "
            + str(reduced)
            + " 字节"
        )


def print_volume_stats(trim_stats, old_html_size, new_html_size, html_baseline):
    old_tiles_bytes = sum(item["old_tiles_base64_bytes"] for item in trim_stats)
    new_png_bytes = sum(item["new_png_data_uri_bytes"] for item in trim_stats)
    print(
        "体积统计：旧 tiles base64 总量 "
        + str(old_tiles_bytes)
        + " 字节 → 新 PNG data URI 总量 "
        + str(new_png_bytes)
        + " 字节"
    )
    print(
        "体积统计：HTML 文件总大小 "
        + str(old_html_size)
        + " 字节 → "
        + str(new_html_size)
        + " 字节（旧值："
        + html_baseline
        + "）"
    )


def serialize_map_data(maps):
    serialized = json.dumps(maps, ensure_ascii=False, allow_nan=False, separators=(",", ":"))
    # JSON 位于 script 元素中，避免地图名中偶然出现 </script> 破坏页面结构。
    return serialized.replace("<", "\\u003c").replace(">", "\\u003e").replace("&", "\\u0026")


def main():
    map_info = load_json(MAP_INFO_PATH)
    tiles_by_id = load_json(TILES_PATH)
    exits_by_id = load_json(EXITS_PATH)
    warnings = []
    trim_stats = []
    legacy_tiles = {}
    maps = build_maps(map_info, tiles_by_id, exits_by_id, warnings, trim_stats, legacy_tiles)
    legacy_maps = [
        {
            "id": map_data["id"],
            "name": map_data["name"],
            "w": map_data["w"],
            "h": map_data["h"],
            "tiles": legacy_tiles[map_data["id"]],
            "exits": map_data["exits"],
        }
        for map_data in maps
    ]
    legacy_html = HTML_TEMPLATE.replace("/*__MAP_DATA__*/", serialize_map_data(legacy_maps))
    html = HTML_TEMPLATE.replace("/*__MAP_DATA__*/", serialize_map_data(maps))
    legacy_html_size = len(legacy_html.encode("utf-8"))
    old_html_size = legacy_html_size
    html_baseline = "旧 tiles 数据同当前模板估算"
    try:
        old_html_size = OUTPUT_PATH.stat().st_size
        html_baseline = "覆盖前的已有文件"
    except FileNotFoundError:
        old_html_size = legacy_html_size
    except OSError as error:
        raise RuntimeError("无法读取旧 HTML 文件大小 " + str(OUTPUT_PATH) + "：" + str(error)) from error
    new_html_size = len(html.encode("utf-8"))
    OUTPUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    try:
        OUTPUT_PATH.write_text(html, encoding="utf-8")
    except OSError as error:
        raise RuntimeError("无法写入 " + str(OUTPUT_PATH) + "：" + str(error)) from error

    print("已生成 " + str(OUTPUT_PATH) + "（" + str(len(maps)) + " 张地图）")
    print_trim_stats(trim_stats)
    print_volume_stats(trim_stats, old_html_size, new_html_size, html_baseline)
    for warning in warnings:
        print("警告：" + warning)


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as error:
        raise SystemExit("地图摆放工具构建失败：" + str(error)) from error
