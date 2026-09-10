// SPDX-License-Identifier: GPL-3.0-or-later
//
// Copyright (C) 2026 Panagiotis Paschalis
//
// This file is part of ui-bin-viewer, a tool of the PESMod project.
//
// ui-bin-viewer is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the
// Free Software Foundation, either version 3 of the License, or (at your
// option) any later version.
//
// ui-bin-viewer is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with ui-bin-viewer.  If not, see <https://www.gnu.org/licenses/>.

import React, { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { UiBinScene } from './UiBinScene.jsx';
import { parseUiBin } from './parser.js';
import './styles.css';

const RECT_SCALE = 1 / 16;
const UV_SCALE = 1 / 4096;

function App() {
  const [parsed, setParsed] = useState(null);
  const [parseError, setParseError] = useState('');
  const [textureFiles, setTextureFiles] = useState({});
  const [selectedKey, setSelectedKey] = useState(null);
  const [expandedKeys, setExpandedKeys] = useState({});
  const [hiddenKeys, setHiddenKeys] = useState([]);
  const [projectionMode, setProjectionMode] = useState('orthographic');
  const [dirty, setDirty] = useState(false);
  const textureFilesRef = useRef(textureFiles);

  useEffect(() => {
    textureFilesRef.current = textureFiles;
  }, [textureFiles]);

  useEffect(() => () => {
    Object.values(textureFilesRef.current).forEach((url) => {
      if (url) URL.revokeObjectURL(url);
    });
  }, []);

  const loadBin = useCallback(async (event) => {
    const file = event.target.files?.[0];
    if (!file) return;

    try {
      const bytes = await file.arrayBuffer();
      const result = parseUiBin(bytes, file.name);
      result.sourceBuffer = bytes.slice(0);
      result.originalSaveSignature = makeSaveSignature(result);
      setParsed(result);
      setSelectedKey(null);
      setExpandedKeys({});
      setHiddenKeys([]);
      setDirty(false);
      setTextureFiles((current) => {
        Object.values(current).forEach((url) => {
          if (url) URL.revokeObjectURL(url);
        });
        return {};
      });
      setParseError('');
      console.groupCollapsed(`[UI BIN Viewer] Parsed ${file.name}`);
      console.log(result);
      console.groupEnd();
    } catch (error) {
      console.error('[UI BIN Viewer] Parse failed', error);
      setParsed(null);
      setSelectedKey(null);
      setExpandedKeys({});
      setHiddenKeys([]);
      setParseError(error instanceof Error ? error.message : String(error));
    }
  }, []);

  const onTextureFile = useCallback((textureName, event) => {
    const file = event.target.files?.[0];
    setTextureFiles((current) => {
      if (current[textureName]) URL.revokeObjectURL(current[textureName]);
      return {
        ...current,
        [textureName]: file ? URL.createObjectURL(file) : undefined,
      };
    });
  }, []);

  const debugLines = useMemo(() => {
    if (!parsed) return [];
    const warnings = parsed.warnings.length
      ? parsed.warnings.map((line) => `warning: ${line}`)
      : ['no parser warnings'];
    return [
      `file: ${parsed.fileName}`,
      `size: ${parsed.byteLength} bytes`,
      `layout kind: ${parsed.layoutKind || 'simple'}`,
      `texture descriptor: ${fmt(parsed.root.textureDescOffset)}`,
      `texture name table: ${fmt(parsed.textures.nameTableOffset)}`,
      `textures: ${parsed.textures.length}`,
      `extra roots: ${parsed.root.extraRoots.map(fmt).join(', ') || 'none'}`,
      ...(parsed.rich ? [
        `rich subresources: ${fmt(parsed.rich.subresourceBank.offset)} table=${fmt(parsed.rich.subresourceBank.tableOffset)} count=${parsed.rich.subresourceBank.subresources.length}`,
        `rich meshes: ${parsed.rich.subresourceBank.subresources.reduce((sum, item) => sum + item.meshes.length, 0)}`,
        `rich attachments: ${parsed.rich.subresourceBank.subresources.reduce((sum, item) => sum + item.attachments.length, 0)}`,
        `rich controls: ${parsed.rich.subresourceBank.subresources.reduce((sum, item) => sum + item.controls.length, 0)}`,
        `rich assets: ${fmt(parsed.rich.assetBank.offset)} table=${fmt(parsed.rich.assetBank.tableOffset)} count=${parsed.rich.assetBank.assets.length}`,
        `rich asset frames: ${parsed.rich.assetBank.assets.reduce((sum, item) => sum + item.frames.length, 0)}`,
        `rich name table: ${fmt(parsed.rich.nameTable.offset)} table=${fmt(parsed.rich.nameTable.tableOffset)} count=${parsed.rich.nameTable.names.length}`,
        `rich names: ${parsed.rich.nameTable.names.map((item) => item.name || '(empty)').join(', ') || 'none'}`,
      ] : []),
      `section directory: ${
        parsed.sectionDir
          ? `${fmt(parsed.sectionDir.offset)} ${parsed.sectionDir.tableShape} table=${fmt(parsed.sectionDir.tableOffset)}`
          : 'not found'
      }`,
      `sections: ${parsed.sections.length}`,
      `draw records: ${parsed.sections.reduce((sum, section) => sum + section.drawRecords.length, 0)}`,
      `mode1 records: ${parsed.sections.reduce((sum, section) => sum + section.mode1Records.length, 0)}`,
      `helper records: ${parsed.sections.reduce((sum, section) => sum + section.helperRecords.length, 0)}`,
      `selected: ${selectedKey || 'none'}`,
      `projection: ${projectionMode}`,
      `hidden nodes: ${hiddenKeys.length}`,
      `dirty: ${dirty ? 'yes' : 'no'}`,
      ...warnings,
    ];
  }, [parsed, selectedKey, projectionMode, hiddenKeys.length, dirty]);

  const selected = useMemo(() => findSelectedItem(parsed, selectedKey), [parsed, selectedKey]);

  const updateItemByKey = useCallback((key, updater) => {
    if (!key) return;
    setParsed((current) => updateSelectedItemInParsed(current, key, updater));
    setDirty(true);
  }, []);

  const updateSelectedItem = useCallback((updater) => {
    updateItemByKey(selectedKey, updater);
  }, [selectedKey, updateItemByKey]);

  const addRecord = useCallback((sectionIndex, type) => {
    let newKey = null;
    setParsed((current) => {
      if (type.startsWith('rich-')) {
        const result = addRichRecordToSubresource(current, sectionIndex, type);
        newKey = result.newKey;
        return result.parsed;
      }
      const section = current?.sections.find((item) => item.index === sectionIndex);
      const table = section ? getRecordTable(section, type) : null;
      const index = table ? nextIndex(table) : 0;
      newKey = makeKey(type, sectionIndex, index);
      return addRecordToSection(current, sectionIndex, type, index);
    });
    if (newKey) setSelectedKey(newKey);
    setExpandedKeys((current) => ({
      ...current,
      ...(type.startsWith('rich-')
        ? { [`rich-subresource:${sectionIndex}`]: true, [`${type}-group:${sectionIndex}`]: true }
        : { [makeKey('section', sectionIndex)]: true, [makeGroupKey(sectionIndex, type)]: true }),
    }));
    setDirty(true);
  }, []);

  const saveModifiedBin = useCallback(() => {
    if (!parsed?.sourceBuffer) return;
    const blob = new Blob([writePatchedBin(parsed)], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = parsed.fileName.replace(/\.(bin|opd)$/i, '') + '.edited.' + (parsed.fileName.toLowerCase().endsWith('.opd') ? 'opd' : 'bin');
    link.click();
    URL.revokeObjectURL(url);
    setDirty(false);
  }, [parsed]);

  const toggleExpanded = useCallback((key) => {
    setExpandedKeys((current) => ({
      ...current,
      [key]: !(current[key] ?? true),
    }));
  }, []);

  const toggleVisible = useCallback((key) => {
    setHiddenKeys((current) => {
      const keys = collectVisibilityKeys(parsed, key);
      const shouldShow = keys.some((item) => current.includes(item));
      if (shouldShow) return current.filter((item) => !keys.includes(item));
      return [...new Set([...current, ...keys])];
    });
  }, [parsed]);

  return (
    <main className="app-shell">
      <aside className="sidebar">
        <section className="panel">
          <h1>UI BIN Viewer</h1>
          <label className="file-picker">
            <span>BIN file</span>
            <input type="file" accept=".bin,.opd,application/octet-stream" onChange={loadBin} />
          </label>
          {parseError ? <p className="error-text">{parseError}</p> : null}
        </section>

        <section className="panel">
          <h2>Elements</h2>
          <ElementTree
            parsed={parsed}
            selectedKey={selectedKey}
            expandedKeys={expandedKeys}
            hiddenKeys={hiddenKeys}
            onSelect={setSelectedKey}
            onToggleExpanded={toggleExpanded}
            onToggleVisible={toggleVisible}
          />
        </section>

        <section className="panel">
          <h2>Selected Element</h2>
          <ElementInfoPanel
            selected={selected}
            onChange={updateSelectedItem}
            onAddRecord={addRecord}
          />
        </section>

        <section className="panel">
          <h2>UV Editor</h2>
          <UvEditor
            selected={selected}
            parsed={parsed}
            textureFiles={textureFiles}
            onChange={updateSelectedItem}
          />
        </section>

        <section className="panel">
          <h2>Save</h2>
          <button className="primary-button" type="button" disabled={!parsed || !dirty} onClick={saveModifiedBin}>
            Save modified BIN
          </button>
          <p className="muted save-note">Exports a patched copy of known section and record fields.</p>
        </section>

        <section className="panel">
          <h2>Controls</h2>
          <div className="segmented-control" role="group" aria-label="Projection mode">
            <button
              type="button"
              className={projectionMode === 'orthographic' ? 'active' : ''}
              onClick={() => setProjectionMode('orthographic')}
            >
              Orthographic
            </button>
            <button
              type="button"
              className={projectionMode === 'perspective' ? 'active' : ''}
              onClick={() => setProjectionMode('perspective')}
            >
              Perspective
            </button>
          </div>
          <dl className="controls">
            <div><dt>W / S</dt><dd>Forward / Back or Pan Y</dd></div>
            <div><dt>A / D</dt><dd>Strafe or Pan X</dd></div>
            <div><dt>Q / E</dt><dd>Down / Up</dd></div>
            <div><dt>Shift</dt><dd>Fast move</dd></div>
            <div><dt>Mouse drag</dt><dd>Look around or Pan</dd></div>
            <div><dt>Object drag</dt><dd>Move in Orthographic</dd></div>
            <div><dt>Handles</dt><dd>Resize selected item</dd></div>
            <div><dt>Wheel</dt><dd>Zoom</dd></div>
            <div><dt>R</dt><dd>Reset camera</dd></div>
          </dl>
        </section>

        <section className="panel texture-panel">
          <h2>Textures</h2>
          {parsed?.textures.length ? (
            parsed.textures.map((texture) => (
              <label className="texture-row" key={`${texture.index}:${texture.name}`}>
                <span>
                  <strong>#{texture.index}</strong>
                  {texture.name || '(empty string)'}
                </span>
                <input
                  type="file"
                  accept="image/png,image/bmp,.png,.bmp"
                  onChange={(event) => onTextureFile(texture.name, event)}
                />
              </label>
            ))
          ) : (
            <p className="muted">Load a BIN to list texture descriptors.</p>
          )}
        </section>

        <section className="panel debug-panel">
          <h2>Debug</h2>
          <pre>{debugLines.join('\n') || 'No file loaded.'}</pre>
        </section>
      </aside>

      <section className="viewport">
        <UiBinScene
          parsed={parsed}
          textureFiles={textureFiles}
          selectedKey={selectedKey}
          projectionMode={projectionMode}
          hiddenKeys={hiddenKeys}
          onEditItem={updateItemByKey}
          onSelect={setSelectedKey}
        />
      </section>
    </main>
  );
}

function ElementTree({
  parsed,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  if (parsed?.rich) {
    return (
      <RichElementTree
        parsed={parsed}
        selectedKey={selectedKey}
        expandedKeys={expandedKeys}
        hiddenKeys={hiddenKeys}
        onSelect={onSelect}
        onToggleExpanded={onToggleExpanded}
        onToggleVisible={onToggleVisible}
      />
    );
  }

  if (!parsed?.sections.length) {
    return <p className="muted">Load a parsed section directory to inspect elements.</p>;
  }

  return (
    <div className="element-tree">
      {parsed.sections.map((section) => {
        const sectionKey = makeKey('section', section.index);
        const sectionExpanded = isExpanded(expandedKeys, sectionKey);
        return (
          <div className="tree-node" key={section.index}>
            <TreeRow
              label={`Section ${section.index}`}
              meta={fmt(section.offset)}
              selected={selectedKey === sectionKey}
              expanded={sectionExpanded}
              hidden={isHidden(hiddenKeys, sectionKey)}
              onSelect={() => onSelect(sectionKey)}
              onToggleExpanded={() => onToggleExpanded(sectionKey)}
              onToggleVisible={() => onToggleVisible(sectionKey)}
            />
            {sectionExpanded ? (
              <>
                <RecordButtons title="Draw" type="draw" records={section.drawRecords} section={section} selectedKey={selectedKey} expandedKeys={expandedKeys} hiddenKeys={hiddenKeys} onSelect={onSelect} onToggleExpanded={onToggleExpanded} onToggleVisible={onToggleVisible} />
                <RecordButtons title="Mode1" type="mode1" records={section.mode1Records} section={section} selectedKey={selectedKey} expandedKeys={expandedKeys} hiddenKeys={hiddenKeys} onSelect={onSelect} onToggleExpanded={onToggleExpanded} onToggleVisible={onToggleVisible} />
                <RecordButtons title="Helpers" type="helper" records={section.helperRecords} section={section} selectedKey={selectedKey} expandedKeys={expandedKeys} hiddenKeys={hiddenKeys} onSelect={onSelect} onToggleExpanded={onToggleExpanded} onToggleVisible={onToggleVisible} />
              </>
            ) : null}
          </div>
        );
      })}
    </div>
  );
}

function RichElementTree({
  parsed,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  const { subresourceBank, assetBank, nameTable } = parsed.rich;
  return (
    <div className="element-tree">
      <RichBankTree
        label={`Subresources (${subresourceBank.subresources.length})`}
        bankKey="rich-bank:subresources"
        meta={fmt(subresourceBank.offset)}
        selectedKey={selectedKey}
        expandedKeys={expandedKeys}
        hiddenKeys={hiddenKeys}
        onSelect={onSelect}
        onToggleExpanded={onToggleExpanded}
        onToggleVisible={onToggleVisible}
      >
        {subresourceBank.subresources.map((subresource) => (
          <RichSubresourceNode
            key={subresource.index}
            subresource={subresource}
            selectedKey={selectedKey}
            expandedKeys={expandedKeys}
            hiddenKeys={hiddenKeys}
            onSelect={onSelect}
            onToggleExpanded={onToggleExpanded}
            onToggleVisible={onToggleVisible}
          />
        ))}
      </RichBankTree>

      <RichBankTree
        label={`Assets (${assetBank.assets.length})`}
        bankKey="rich-bank:assets"
        meta={fmt(assetBank.offset)}
        selectedKey={selectedKey}
        expandedKeys={expandedKeys}
        hiddenKeys={hiddenKeys}
        onSelect={onSelect}
        onToggleExpanded={onToggleExpanded}
        onToggleVisible={onToggleVisible}
      >
        {assetBank.assets.map((asset) => (
          <RichAssetNode
            key={asset.index}
            asset={asset}
            selectedKey={selectedKey}
            expandedKeys={expandedKeys}
            hiddenKeys={hiddenKeys}
            onSelect={onSelect}
            onToggleExpanded={onToggleExpanded}
            onToggleVisible={onToggleVisible}
          />
        ))}
      </RichBankTree>

      <RichBankTree
        label={`Names (${nameTable.names.length})`}
        bankKey="rich-bank:names"
        meta={fmt(nameTable.offset)}
        selectedKey={selectedKey}
        expandedKeys={expandedKeys}
        hiddenKeys={hiddenKeys}
        onSelect={onSelect}
        onToggleExpanded={onToggleExpanded}
        onToggleVisible={onToggleVisible}
      >
        {nameTable.names.map((name) => {
          const key = `rich-name:${name.index}`;
          return (
            <TreeRow
              key={key}
              label={`#${name.index} ${name.name || '(empty)'}`}
              meta={fmt(name.nameOffset)}
              selected={selectedKey === key}
              hidden={isHidden(hiddenKeys, key)}
              inheritedHidden={isHidden(hiddenKeys, 'rich-bank:names')}
              onSelect={() => onSelect(key)}
              onToggleVisible={() => onToggleVisible(key)}
            />
          );
        })}
      </RichBankTree>
    </div>
  );
}

function RichBankTree({
  label,
  bankKey,
  meta,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
  children,
}) {
  const expanded = isExpanded(expandedKeys, bankKey);
  const hidden = isHidden(hiddenKeys, bankKey);
  return (
    <div className="tree-node">
      <TreeRow
        label={label}
        meta={meta}
        selected={selectedKey === bankKey}
        expanded={expanded}
        hidden={hidden}
        onSelect={() => onSelect(bankKey)}
        onToggleExpanded={() => onToggleExpanded(bankKey)}
        onToggleVisible={() => onToggleVisible(bankKey)}
      />
      {expanded ? <div className="tree-group">{children}</div> : null}
    </div>
  );
}

function RichSubresourceNode({
  subresource,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  const key = `rich-subresource:${subresource.index}`;
  const expanded = isExpanded(expandedKeys, key);
  const hidden = isHidden(hiddenKeys, key);
  return (
    <div>
      <TreeRow
        label={`Subresource ${subresource.index}`}
        meta={fmt(subresource.offset)}
        selected={selectedKey === key}
        expanded={expanded}
        hidden={hidden}
        inheritedHidden={isHidden(hiddenKeys, 'rich-bank:subresources')}
        onSelect={() => onSelect(key)}
        onToggleExpanded={() => onToggleExpanded(key)}
        onToggleVisible={() => onToggleVisible(key)}
      />
      {expanded ? (
        <div className="tree-group">
          <RichRecordList title="Meshes" type="rich-mesh" parentIndex={subresource.index} records={subresource.meshes} selectedKey={selectedKey} hiddenKeys={hiddenKeys} inheritedHidden={hidden} onSelect={onSelect} onToggleVisible={onToggleVisible} />
          <RichRecordList title="Attachments" type="rich-attachment" parentIndex={subresource.index} records={subresource.attachments} selectedKey={selectedKey} hiddenKeys={hiddenKeys} inheritedHidden={hidden} onSelect={onSelect} onToggleVisible={onToggleVisible} />
          <RichRecordList title="Controls" type="rich-control" parentIndex={subresource.index} records={subresource.controls} selectedKey={selectedKey} hiddenKeys={hiddenKeys} inheritedHidden={hidden} onSelect={onSelect} onToggleVisible={onToggleVisible} />
        </div>
      ) : null}
    </div>
  );
}

function RichRecordList({
  title,
  type,
  parentIndex,
  records,
  selectedKey,
  hiddenKeys,
  inheritedHidden,
  onSelect,
  onToggleVisible,
}) {
  const groupKey = `${type}-group:${parentIndex}`;
  const groupHidden = isHidden(hiddenKeys, groupKey);
  return (
    <div className="tree-group">
      <TreeRow
        label={`${title} (${records.length})`}
        meta=""
        selected={false}
        hidden={groupHidden}
        inheritedHidden={inheritedHidden}
        onSelect={() => onToggleVisible(groupKey)}
        onToggleVisible={() => onToggleVisible(groupKey)}
      />
      {records.map((record) => {
        const key = `${type}:${parentIndex}:${record.index}`;
        return (
          <TreeRow
            key={key}
            label={`#${record.index}`}
            meta={fmt(record.offset)}
            selected={selectedKey === key}
            hidden={isHidden(hiddenKeys, key)}
            inheritedHidden={inheritedHidden || groupHidden}
            onSelect={() => onSelect(key)}
            onToggleVisible={() => onToggleVisible(key)}
          />
        );
      })}
    </div>
  );
}

function RichAssetNode({
  asset,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  const key = `rich-asset:${asset.index}`;
  const expanded = isExpanded(expandedKeys, key);
  const hidden = isHidden(hiddenKeys, key);
  return (
    <div>
      <TreeRow
        label={`Asset ${asset.index}`}
        meta={fmt(asset.offset)}
        selected={selectedKey === key}
        expanded={expanded}
        hidden={hidden}
        inheritedHidden={isHidden(hiddenKeys, 'rich-bank:assets')}
        onSelect={() => onSelect(key)}
        onToggleExpanded={() => onToggleExpanded(key)}
        onToggleVisible={() => onToggleVisible(key)}
      />
      {expanded ? (
        <div className="tree-group">
          {asset.frames.map((frame) => {
            const frameKey = `rich-frame:${asset.index}:${frame.index}`;
            return (
              <TreeRow
                key={frameKey}
                label={`Frame ${frame.index}`}
                meta={`sub ${frame.subresourceIndex}`}
                selected={selectedKey === frameKey}
                hidden={isHidden(hiddenKeys, frameKey)}
                inheritedHidden={hidden}
                onSelect={() => onSelect(frameKey)}
                onToggleVisible={() => onToggleVisible(frameKey)}
              />
            );
          })}
          {asset.textureSlots.map((slot, index) => {
            const slotKey = `rich-texture-slot:${asset.index}:${index}`;
            return (
              <TreeRow
                key={slotKey}
                label={`Texture slot ${index}`}
                meta={`${slot}`}
                selected={selectedKey === slotKey}
                hidden={isHidden(hiddenKeys, slotKey)}
                inheritedHidden={hidden}
                onSelect={() => onSelect(slotKey)}
                onToggleVisible={() => onToggleVisible(slotKey)}
              />
            );
          })}
        </div>
      ) : null}
    </div>
  );
}

function RecordButtons({
  title,
  type,
  records,
  section,
  selectedKey,
  expandedKeys,
  hiddenKeys,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  const sectionKey = makeKey('section', section.index);
  const groupKey = makeGroupKey(section.index, type);
  const groupExpanded = isExpanded(expandedKeys, groupKey);
  const groupHidden = isHidden(hiddenKeys, groupKey);
  const sectionHidden = isHidden(hiddenKeys, sectionKey);

  return (
    <div className="tree-group">
      <TreeRow
        label={`${title} (${records.length})`}
        meta=""
        selected={false}
        expanded={groupExpanded}
        hidden={groupHidden}
        inheritedHidden={sectionHidden}
        onSelect={() => onToggleExpanded(groupKey)}
        onToggleExpanded={() => onToggleExpanded(groupKey)}
        onToggleVisible={() => onToggleVisible(groupKey)}
      />
      {groupExpanded ? records.map((record) => {
        const key = makeKey(type, section.index, record.index);
        const hidden = isHidden(hiddenKeys, key);
        return (
          <TreeRow
            key={key}
            label={`#${record.index} id ${record.recordId ?? record.helperId ?? '-'}`}
            meta={fmt(record.offset)}
            selected={selectedKey === key}
            hidden={hidden}
            inheritedHidden={sectionHidden || groupHidden}
            onSelect={() => onSelect(key)}
            onToggleVisible={() => onToggleVisible(key)}
          />
        );
      }) : null}
    </div>
  );
}

function TreeRow({
  label,
  meta,
  selected,
  expanded,
  hidden,
  inheritedHidden,
  onSelect,
  onToggleExpanded,
  onToggleVisible,
}) {
  const canExpand = typeof expanded === 'boolean';
  const hiddenClass = hidden || inheritedHidden ? ' hidden' : '';
  return (
    <div className={`tree-row${selected ? ' selected' : ''}${hiddenClass}`}>
      {canExpand ? (
        <button className="tree-icon-button" type="button" onClick={onToggleExpanded}>
          {expanded ? 'Collapse' : 'Expand'}
        </button>
      ) : (
        <span className="tree-icon-spacer" />
      )}
      <button className="tree-item" type="button" onClick={onSelect}>
        <span>{label}</span>
        {meta ? <strong>{meta}</strong> : null}
      </button>
      <button className="tree-icon-button" type="button" onClick={onToggleVisible}>
        {hidden ? 'Show' : 'Hide'}
      </button>
    </div>
  );
}

function ElementInfoPanel({ selected, onChange, onAddRecord }) {
  if (!selected) {
    return <p className="muted">Click an element in the viewport or tree.</p>;
  }

  const { type, section, item } = selected;
  if (type.startsWith('rich-')) {
    return <RichInfoPanel selected={selected} onChange={onChange} onAddRecord={onAddRecord} />;
  }

  if (type === 'section') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Section header" />
        <InfoLine label="Offset" value={fmt(section.offset)} />
        <NumberField label="Draw count" value={section.header.drawCount} min={0} max={255} onChange={(value) => onChange((draft) => updateHeader(draft, 'drawCount', value))} />
        <NumberField label="Mode1 count" value={section.header.mode1Count} min={0} max={255} onChange={(value) => onChange((draft) => updateHeader(draft, 'mode1Count', value))} />
        <NumberField label="Helper count" value={section.header.helperCount} min={0} max={255} onChange={(value) => onChange((draft) => updateHeader(draft, 'helperCount', value))} />
        <NumberField label="Count flags 03" value={section.header.countFlags03} min={0} max={255} onChange={(value) => onChange((draft) => updateHeader(draft, 'countFlags03', value))} />
        <NumberField label="Draw ptr" value={section.header.rawDrawPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => updateHeader(draft, 'rawDrawPtr', value))} />
        <NumberField label="Mode1 ptr" value={section.header.rawMode1Ptr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => updateHeader(draft, 'rawMode1Ptr', value))} />
        <NumberField label="Helper ptr" value={section.header.rawHelperPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => updateHeader(draft, 'rawHelperPtr', value))} />
        <NumberField label="Flags 10" value={section.header.flags10} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => updateHeader(draft, 'flags10', value))} />
        <div className="action-row">
          <button type="button" onClick={() => onAddRecord(section.index, 'draw')}>Add draw</button>
          <button type="button" onClick={() => onAddRecord(section.index, 'helper')}>Add helper</button>
        </div>
      </div>
    );
  }

  if (type === 'helper') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Helper record" />
        <InfoLine label="Section" value={section.index} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Helper type" value={item.helperType} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, helperType: value }))} />
        <NumberField label="Bank / slot" value={item.bankOrSlot} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, bankOrSlot: value }))} />
        <NumberField label="Helper ID" value={item.helperId} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, helperId: value }))} />
        <RectFields item={item} onChange={onChange} />
      </div>
    );
  }

  const isDraw = type === 'draw';
  const typeLabel = isDraw ? 'Draw record' : 'Mode1 record';
  return (
    <div className="element-editor">
      <InfoLine label="Type" value={typeLabel} />
      <InfoLine label="Section" value={section.index} />
      <InfoLine label="Offset" value={fmt(item.offset)} />
      <NumberField label="Record type" value={item.recordType} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, recordType: value }))} />
      {isDraw ? (
        <NumberField label="Texture slot" value={item.textureSlot} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, textureSlot: value }))} />
      ) : (
        <NumberField label="Bank / slot" value={item.bankOrSlot} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, bankOrSlot: value }))} />
      )}
      <NumberField label="Record ID" value={item.recordId} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, recordId: value }))} />
      <RectFields item={item} onChange={onChange} />
      <div className="color-grid">
        {item.colors.map((color, index) => (
          <label key={index}>
            C{index}
            <input
              type="color"
              value={rgbToHex(color)}
              onChange={(event) => onChange((draft) => replaceColor(draft, index, event.target.value))}
            />
          </label>
        ))}
      </div>
      <div className="alpha-grid">
        {item.colors.map((color, index) => (
          <NumberField
            key={index}
            label={`A${index}`}
            value={color.a}
            min={0}
            max={255}
            onChange={(value) => onChange((draft) => replaceAlpha(draft, index, value))}
          />
        ))}
      </div>
    </div>
  );
}

function RichInfoPanel({ selected, onChange, onAddRecord }) {
  const { type, item, parent } = selected;

  if (type === 'rich-bank') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value={`Rich ${selected.bankType} bank`} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Count" value={item.count} min={0} max={4096} onChange={(value) => onChange((draft) => ({ ...draft, count: value }))} />
        <NumberField label="Table ptr" value={item.rawTablePtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawTablePtr: value }))} />
      </div>
    );
  }

  if (type === 'rich-subresource') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich subresource" />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Mesh count" value={item.meshCount} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, meshCount: value, subrecordCount: value }))} />
        <NumberField label="Attachment count" value={item.attachmentCount} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, attachmentCount: value, field01: value }))} />
        <NumberField label="Control count" value={item.controlCount} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, controlCount: value, field02: value }))} />
        <NumberField label="Field 03" value={item.field03} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, field03: value }))} />
        <VecFields title="Origin" value={item.origin} keys={['rawX', 'rawY', 'rawZ', 'rawW']} onChange={(key, value) => onChange((draft) => ({ ...draft, origin: replaceVecValue(draft.origin, key, value) }))} />
        <NumberField label="Mesh table ptr" value={item.rawMeshTablePtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawMeshTablePtr: value, rawSubrecordsPtr: value }))} />
        <NumberField label="Attachments ptr" value={item.rawAttachmentsPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawAttachmentsPtr: value, rawPtr10: value }))} />
        <NumberField label="Controls ptr" value={item.rawControlsPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawControlsPtr: value, rawPtr14: value }))} />
        <div className="action-row">
          <button type="button" onClick={() => onAddRecord(item.index, 'rich-mesh')}>Add mesh</button>
          <button type="button" onClick={() => onAddRecord(item.index, 'rich-attachment')}>Add attachment</button>
          <button type="button" onClick={() => onAddRecord(item.index, 'rich-control')}>Add control</button>
        </div>
      </div>
    );
  }

  if (type === 'rich-mesh') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich mesh" />
        <InfoLine label="Subresource" value={parent.index} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Flags 00" value={item.flags00} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, flags00: value, field00: value }))} />
        <NumberField label="Strip/cull" value={item.stripCullFlag} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, stripCullFlag: value, field01: value }))} />
        <NumberField label="Has colors ptr" value={item.hasPtr18} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, hasPtr18: value }))} />
        <NumberField label="Texture slot" value={item.textureSlot} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, textureSlot: value, field03: value }))} />
        <NumberField label="Vertex count" value={item.vertexCount} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, vertexCount: value }))} />
        <RichColorFields label="Base color" color={item.baseColor} onChange={(channel, value) => onChange((draft) => ({ ...draft, baseColor: replaceRichColorByte(draft.baseColor, channel, value) }))} />
        <VecFields title="Local offset" value={item.localOffset} keys={['rawX', 'rawY', 'rawZ']} onChange={(key, value) => onChange((draft) => ({ ...draft, localOffset: replaceVecValue(draft.localOffset, key, value) }))} />
        <NumberField label="Positions ptr" value={item.rawPositionsPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawPositionsPtr: value, rawPtr10: value }))} />
        <NumberField label="UV ptr" value={item.rawUvPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawUvPtr: value, rawPtr14: value }))} />
        <NumberField label="Colors ptr" value={item.rawColorsPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawColorsPtr: value, rawPtr18: value }))} />
        <RichVertexFields mesh={item} onChange={onChange} />
      </div>
    );
  }

  if (type === 'rich-attachment') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich attachment" />
        <InfoLine label="Subresource" value={parent.index} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Type" value={item.type} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, type: value }))} />
        <NumberField label="Texture slot" value={item.textureSlot} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, textureSlot: value }))} />
        <NumberField label="Flags 02" value={item.flags02} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, flags02: value }))} />
        <NumberField label="Flags 03" value={item.flags03} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, flags03: value }))} />
        <VecFields title="Transform" value={item.transform} keys={['rawX', 'rawY', 'rawZ', 'rawW']} onChange={(key, value) => onChange((draft) => ({ ...draft, transform: replaceVecValue(draft.transform, key, value) }))} />
        <NumberField label="Field 0C" value={item.field0c} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, field0c: value }))} />
        <NumberField label="Mode 0E" value={item.mode0e} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, mode0e: value }))} />
        <NumberField label="Palette" value={item.paletteIndex} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, paletteIndex: value }))} />
        <NumberField label="Attachment ID" value={item.attachmentId} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, attachmentId: value }))} />
        <NumberField label="Field 12" value={item.field12} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, field12: value }))} />
        <NumberField label="Lookup key" value={item.lookupKey} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, lookupKey: value }))} />
      </div>
    );
  }

  if (type === 'rich-control') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich control" />
        <InfoLine label="Subresource" value={parent.index} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Type" value={item.type} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, type: value }))} />
        <NumberField label="Field 01" value={item.field01} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, field01: value }))} />
        <NumberField label="Control ID" value={item.controlId} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, controlId: value }))} />
        <RectFields item={item.rect} onChange={(updater) => onChange((draft) => ({ ...draft, rect: updater(draft.rect) }))} />
      </div>
    );
  }

  if (type === 'rich-asset') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich asset" />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Frame count" value={item.frameCount} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, frameCount: value }))} />
        <NumberField label="Field 02" value={item.field02} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, field02: value }))} />
        <NumberField label="Texture slots" value={item.textureSlotCount} min={0} max={255} onChange={(value) => onChange((draft) => ({ ...draft, textureSlotCount: value }))} />
        <NumberField label="Frames ptr" value={item.rawFramesPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawFramesPtr: value, rawPtr04: value }))} />
        <NumberField label="Texture slots ptr" value={item.rawTextureSlotsPtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawTextureSlotsPtr: value, rawPtr08: value }))} />
      </div>
    );
  }

  if (type === 'rich-frame') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich asset frame" />
        <InfoLine label="Asset" value={parent.index} />
        <InfoLine label="Offset" value={fmt(item.offset)} />
        <NumberField label="Subresource" value={item.subresourceIndex} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, subresourceIndex: value }))} />
        <NumberField label="Frame flags" value={item.frameFlags} min={0} max={65535} onChange={(value) => onChange((draft) => ({ ...draft, frameFlags: value }))} />
      </div>
    );
  }

  if (type === 'rich-texture-slot') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich texture slot" />
        <InfoLine label="Asset" value={parent.index} />
        <InfoLine label="Index" value={selected.slotIndex} />
        <NumberField label="Texture index" value={item.value} min={0} max={65535} onChange={(value) => onChange(() => ({ value }))} />
      </div>
    );
  }

  if (type === 'rich-name') {
    return (
      <div className="element-editor">
        <InfoLine label="Type" value="Rich name" />
        <InfoLine label="Offset" value={fmt(item.nameOffset)} />
        <TextField label="Name" value={item.name} onChange={(value) => onChange((draft) => ({ ...draft, name: value }))} />
        <NumberField label="Name ptr" value={item.rawNamePtr} min={0} max={0xffffffff} onChange={(value) => onChange((draft) => ({ ...draft, rawNamePtr: value }))} />
      </div>
    );
  }

  return <p className="muted">No editor is available for this rich item yet.</p>;
}

function VecFields({ title, value, keys, onChange }) {
  return (
    <>
      <InfoLine label={title} value="" />
      {keys.map((key) => (
        <NumberField key={key} label={key} value={value?.[key] ?? 0} onChange={(next) => onChange(key, next)} />
      ))}
    </>
  );
}

function RichColorFields({ label, color, onChange }) {
  return (
    <>
      <InfoLine label={label} value="" />
      {['r', 'g', 'b', 'a'].map((channel, index) => (
        <NumberField key={channel} label={`raw ${channel.toUpperCase()}`} value={color?.raw?.[index] ?? 0} min={0} max={255} onChange={(value) => onChange(index, value)} />
      ))}
    </>
  );
}

function RichVertexFields({ mesh, onChange }) {
  return (
    <div className="rich-array-editor">
      <InfoLine label="Vertices" value={mesh.positions.length} />
      {mesh.positions.map((position, index) => (
        <div className="array-item" key={`pos:${index}`}>
          <InfoLine label={`Vertex ${index}`} value="" />
          {['rawX', 'rawY', 'rawZ'].map((key) => (
            <NumberField key={key} label={`P ${key}`} value={position[key]} onChange={(value) => onChange((draft) => replaceArrayVec(draft, 'positions', index, key, value))} />
          ))}
          <NumberField label="U" value={mesh.uv[index]?.rawU ?? 0} onChange={(value) => onChange((draft) => replaceArrayUv(draft, index, 'rawU', value))} />
          <NumberField label="V" value={mesh.uv[index]?.rawV ?? 0} onChange={(value) => onChange((draft) => replaceArrayUv(draft, index, 'rawV', value))} />
          {mesh.colors[index] ? (
            <RichColorFields label="Color" color={mesh.colors[index]} onChange={(channel, value) => onChange((draft) => replaceArrayColorByte(draft, index, channel, value))} />
          ) : null}
        </div>
      ))}
    </div>
  );
}

function TextField({ label, value, onChange }) {
  return (
    <label className="text-field">
      <span>{label}</span>
      <input type="text" value={value ?? ''} onChange={(event) => onChange(event.target.value)} />
    </label>
  );
}

function collectVisibilityKeys(parsed, key) {
  if (!parsed || !key) return [key];
  const keys = new Set([key]);

  if (key === 'rich-bank:subresources') {
    parsed.rich?.subresourceBank.subresources.forEach((subresource) => addRichSubresourceVisibilityKeys(keys, subresource));
  } else if (key === 'rich-bank:assets') {
    parsed.rich?.assetBank.assets.forEach((asset) => addRichAssetVisibilityKeys(keys, asset));
  } else if (key === 'rich-bank:names') {
    parsed.rich?.nameTable.names.forEach((name) => keys.add(`rich-name:${name.index}`));
  } else if (key.startsWith('rich-subresource:')) {
    const subresource = parsed.rich?.subresourceBank.subresources.find((item) => item.index === Number(key.split(':')[1]));
    if (subresource) addRichSubresourceVisibilityKeys(keys, subresource);
  } else if (key.startsWith('rich-asset:')) {
    const asset = parsed.rich?.assetBank.assets.find((item) => item.index === Number(key.split(':')[1]));
    if (asset) addRichAssetVisibilityKeys(keys, asset);
  } else if (key.startsWith('rich-mesh-group:') || key.startsWith('rich-attachment-group:') || key.startsWith('rich-control-group:')) {
    const [type, parentText] = key.split(':');
    const subresource = parsed.rich?.subresourceBank.subresources.find((item) => item.index === Number(parentText));
    const childType = type.replace('-group', '');
    const tableName = { 'rich-mesh': 'meshes', 'rich-attachment': 'attachments', 'rich-control': 'controls' }[childType];
    subresource?.[tableName]?.forEach((item) => keys.add(`${childType}:${subresource.index}:${item.index}`));
  } else if (key.startsWith('section:')) {
    const section = parsed.sections.find((item) => item.index === Number(key.split(':')[1]));
    if (section) addSectionVisibilityKeys(keys, section);
  } else if (key.startsWith('group:')) {
    const [, sectionText, type] = key.split(':');
    const section = parsed.sections.find((item) => item.index === Number(sectionText));
    getRecordTable(section, type)?.forEach((record) => keys.add(makeKey(type, section.index, record.index)));
  }

  return [...keys];
}

function addSectionVisibilityKeys(keys, section) {
  ['draw', 'mode1', 'helper'].forEach((type) => {
    keys.add(makeGroupKey(section.index, type));
    getRecordTable(section, type)?.forEach((record) => keys.add(makeKey(type, section.index, record.index)));
  });
}

function addRichSubresourceVisibilityKeys(keys, subresource) {
  keys.add(`rich-subresource:${subresource.index}`);
  ['rich-mesh', 'rich-attachment', 'rich-control'].forEach((type) => {
    const tableName = { 'rich-mesh': 'meshes', 'rich-attachment': 'attachments', 'rich-control': 'controls' }[type];
    keys.add(`${type}-group:${subresource.index}`);
    subresource[tableName].forEach((item) => keys.add(`${type}:${subresource.index}:${item.index}`));
  });
}

function addRichAssetVisibilityKeys(keys, asset) {
  keys.add(`rich-asset:${asset.index}`);
  asset.frames.forEach((frame) => keys.add(`rich-frame:${asset.index}:${frame.index}`));
  asset.textureSlots.forEach((slot, index) => keys.add(`rich-texture-slot:${asset.index}:${index}`));
}

function InfoLine({ label, value }) {
  return (
    <div className="info-line">
      <span>{label}</span>
      <strong>{value}</strong>
    </div>
  );
}

function RectFields({ item, onChange }) {
  return (
    <>
      <NumberField label="Raw X" value={item.rawX} onChange={(value) => onChange((draft) => ({ ...draft, rawX: value, x: value * RECT_SCALE }))} />
      <NumberField label="Raw Y" value={item.rawY} onChange={(value) => onChange((draft) => ({ ...draft, rawY: value, y: value * RECT_SCALE }))} />
      <NumberField label="Raw depth" value={item.rawDepth} onChange={(value) => onChange((draft) => ({ ...draft, rawDepth: value, z: -value }))} />
      <NumberField label="Raw width" value={item.rawWidth} onChange={(value) => onChange((draft) => ({ ...draft, rawWidth: value, width: value * RECT_SCALE }))} />
      <NumberField label="Raw height" value={item.rawHeight} onChange={(value) => onChange((draft) => ({ ...draft, rawHeight: value, height: value * RECT_SCALE }))} />
    </>
  );
}

function UvEditor({ selected, parsed, textureFiles, onChange }) {
  const [dragIndex, setDragIndex] = useState(null);
  const svgRef = useRef(null);

  if (!selected || (selected.type !== 'draw' && selected.type !== 'rich-mesh')) {
    return <p className="muted">Select a draw element or rich mesh to edit UV vertices.</p>;
  }

  const record = selected.item;
  const texture = parsed?.textures.find((item) => item.index === record.textureSlot);
  const textureUrl = texture ? textureFiles[texture.name] : null;
  const textureName = texture?.name || `texture_${record.textureSlot}`;
  const points = record.uv.map((uv) => `${clamp(uv.u, 0, 1)},${clamp(uv.v, 0, 1)}`).join(' ');

  const updateFromPointer = (event, index) => {
    const rect = svgRef.current.getBoundingClientRect();
    const u = clamp((event.clientX - rect.left) / rect.width, 0, 1);
    const v = clamp((event.clientY - rect.top) / rect.height, 0, 1);
    onChange((draft) => (selected.type === 'rich-mesh'
      ? replaceRichUv(draft, index, u, v)
      : replaceUv(draft, index, u, v)));
  };

  return (
    <div className="uv-editor">
      <div className="uv-canvas" style={textureUrl ? { backgroundImage: `url("${textureUrl}")` } : undefined}>
        {!textureUrl ? <span>{textureName}</span> : null}
        <svg
          ref={svgRef}
          viewBox="0 0 1 1"
          preserveAspectRatio="none"
          onPointerMove={(event) => {
            if (dragIndex !== null) updateFromPointer(event, dragIndex);
          }}
          onPointerUp={() => setDragIndex(null)}
          onPointerLeave={() => setDragIndex(null)}
        >
          <polygon points={points} />
          {record.uv.map((uv, index) => (
            <circle
              key={index}
              cx={clamp(uv.u, 0, 1)}
              cy={clamp(uv.v, 0, 1)}
              r="0.025"
              onPointerDown={(event) => {
                event.currentTarget.setPointerCapture(event.pointerId);
                setDragIndex(index);
                updateFromPointer(event, index);
              }}
            />
          ))}
        </svg>
      </div>
      <div className="uv-fields">
        {record.uv.map((uv, index) => (
          <React.Fragment key={index}>
            <NumberField label={`U${index}`} value={uv.rawU} onChange={(value) => onChange((draft) => (selected.type === 'rich-mesh' ? replaceArrayUv(draft, index, 'rawU', value) : replaceRawUv(draft, index, 'rawU', value)))} />
            <NumberField label={`V${index}`} value={uv.rawV} onChange={(value) => onChange((draft) => (selected.type === 'rich-mesh' ? replaceArrayUv(draft, index, 'rawV', value) : replaceRawUv(draft, index, 'rawV', value)))} />
          </React.Fragment>
        ))}
      </div>
    </div>
  );
}

function NumberField({ label, value, min, max, onChange }) {
  return (
    <label className="number-field">
      <span>{label}</span>
      <input
        type="number"
        value={Number.isFinite(value) ? value : 0}
        min={min}
        max={max}
        onChange={(event) => onChange(clampNumber(Number(event.target.value), min, max))}
      />
    </label>
  );
}

function findSelectedItem(parsed, key) {
  if (!parsed || !key) return null;
  const richSelected = findSelectedRichItem(parsed, key);
  if (richSelected) return richSelected;

  const [type, sectionText, itemText] = key.split(':');
  const sectionIndex = Number(sectionText);
  const section = parsed.sections.find((item) => item.index === sectionIndex);
  if (!section) return null;
  if (type === 'section') return { type, section, item: section };

  const itemIndex = Number(itemText);
  const table = getRecordTable(section, type);
  const item = table?.find((record) => record.index === itemIndex);
  return item ? { type, section, item } : null;
}

function findSelectedRichItem(parsed, key) {
  if (!parsed?.rich || !key.startsWith('rich-')) return null;
  const parts = key.split(':');
  const [type, firstText, secondText] = parts;

  if (type === 'rich-bank') {
    if (firstText === 'subresources') return { type, bankType: 'subresource', item: parsed.rich.subresourceBank };
    if (firstText === 'assets') return { type, bankType: 'asset', item: parsed.rich.assetBank };
    if (firstText === 'names') return { type, bankType: 'name', item: parsed.rich.nameTable };
    return null;
  }

  if (type === 'rich-name') {
    const item = parsed.rich.nameTable.names.find((name) => name.index === Number(firstText));
    return item ? { type, item } : null;
  }

  if (type === 'rich-asset') {
    const item = parsed.rich.assetBank.assets.find((asset) => asset.index === Number(firstText));
    return item ? { type, item } : null;
  }

  if (type === 'rich-frame' || type === 'rich-texture-slot') {
    const asset = parsed.rich.assetBank.assets.find((item) => item.index === Number(firstText));
    if (!asset) return null;
    const itemIndex = Number(secondText);
    if (type === 'rich-frame') {
      const item = asset.frames.find((frame) => frame.index === itemIndex);
      return item ? { type, parent: asset, item } : null;
    }
    if (itemIndex < 0 || itemIndex >= asset.textureSlots.length) return null;
    return { type, parent: asset, slotIndex: itemIndex, item: { value: asset.textureSlots[itemIndex] } };
  }

  const subresource = parsed.rich.subresourceBank.subresources.find((item) => item.index === Number(firstText));
  if (!subresource) return null;
  if (type === 'rich-subresource') return { type, item: subresource };

  const itemIndex = Number(secondText);
  if (type === 'rich-mesh') {
    const item = subresource.meshes.find((mesh) => mesh.index === itemIndex);
    return item ? { type, parent: subresource, item } : null;
  }
  if (type === 'rich-attachment') {
    const item = subresource.attachments.find((attachment) => attachment.index === itemIndex);
    return item ? { type, parent: subresource, item } : null;
  }
  if (type === 'rich-control') {
    const item = subresource.controls.find((control) => control.index === itemIndex);
    return item ? { type, parent: subresource, item } : null;
  }

  return null;
}

function updateSelectedItemInParsed(parsed, key, updater) {
  if (!parsed) return parsed;
  if (key.startsWith('rich-')) return updateSelectedRichItemInParsed(parsed, key, updater);

  const [type, sectionText, itemText] = key.split(':');
  const sectionIndex = Number(sectionText);
  const itemIndex = Number(itemText);
  return {
    ...parsed,
    sections: parsed.sections.map((section) => {
      if (section.index !== sectionIndex) return section;
      if (type === 'section') {
        return updater({
          ...section,
          header: { ...section.header },
          drawRecords: section.drawRecords,
          mode1Records: section.mode1Records,
          helperRecords: section.helperRecords,
        });
      }

      const tableName = getRecordTableName(type);
      if (!tableName) return section;
      return {
        ...section,
        [tableName]: section[tableName].map((record) => {
          if (record.index !== itemIndex) return record;
          return cloneAndUpdateRecord(record, updater);
        }),
      };
    }),
  };
}

function updateSelectedRichItemInParsed(parsed, key, updater) {
  if (!parsed?.rich) return parsed;
  const [type, firstText, secondText] = key.split(':');

  if (type === 'rich-bank') {
    return updateRichBank(parsed, firstText, updater);
  }

  if (type === 'rich-name') {
    const nameIndex = Number(firstText);
    return updateRichName(parsed, nameIndex, updater);
  }

  if (type === 'rich-asset') {
    const assetIndex = Number(firstText);
    return updateRichAsset(parsed, assetIndex, updater);
  }

  if (type === 'rich-frame') {
    return updateRichAsset(parsed, Number(firstText), (asset) => ({
      ...asset,
      frames: asset.frames.map((frame) => (
        frame.index === Number(secondText) ? updater({ ...frame }) : frame
      )),
    }));
  }

  if (type === 'rich-texture-slot') {
    return updateRichAsset(parsed, Number(firstText), (asset) => ({
      ...asset,
      textureSlots: asset.textureSlots.map((slot, index) => (
        index === Number(secondText) ? updater({ value: slot }).value : slot
      )),
    }));
  }

  const subresourceIndex = Number(firstText);
  if (type === 'rich-subresource') {
    return updateRichSubresource(parsed, subresourceIndex, updater);
  }

  const itemIndex = Number(secondText);
  const tableName = {
    'rich-mesh': 'meshes',
    'rich-attachment': 'attachments',
    'rich-control': 'controls',
  }[type];
  if (!tableName) return parsed;

  return updateRichSubresource(parsed, subresourceIndex, (subresource) => ({
    ...subresource,
    [tableName]: subresource[tableName].map((item) => (
      item.index === itemIndex ? cloneAndUpdateRichItem(item, updater) : item
    )),
  }));
}

function updateRichBank(parsed, bankKey, updater) {
  const rich = { ...parsed.rich };
  if (bankKey === 'subresources') {
    rich.subresourceBank = updater({ ...rich.subresourceBank, subresources: rich.subresourceBank.subresources });
    rich.elementBank = { ...rich.subresourceBank, elements: rich.subresourceBank.subresources };
  } else if (bankKey === 'assets') {
    rich.assetBank = updater({ ...rich.assetBank, assets: rich.assetBank.assets });
    rich.groupBank = { ...rich.assetBank, groups: rich.assetBank.assets };
  } else if (bankKey === 'names') {
    rich.nameTable = updater({ ...rich.nameTable, names: rich.nameTable.names });
  }
  return { ...parsed, rich };
}

function updateRichSubresource(parsed, subresourceIndex, updater) {
  const subresources = parsed.rich.subresourceBank.subresources.map((subresource) => (
    subresource.index === subresourceIndex ? cloneAndUpdateRichItem(subresource, updater) : subresource
  ));
  const subresourceBank = { ...parsed.rich.subresourceBank, subresources };
  return {
    ...parsed,
    rich: {
      ...parsed.rich,
      subresourceBank,
      elementBank: { ...subresourceBank, elements: subresources },
    },
  };
}

function updateRichAsset(parsed, assetIndex, updater) {
  const assets = parsed.rich.assetBank.assets.map((asset) => (
    asset.index === assetIndex ? cloneAndUpdateRichItem(asset, updater) : asset
  ));
  const assetBank = { ...parsed.rich.assetBank, assets };
  return {
    ...parsed,
    rich: {
      ...parsed.rich,
      assetBank,
      groupBank: { ...assetBank, groups: assets },
    },
  };
}

function updateRichName(parsed, nameIndex, updater) {
  return {
    ...parsed,
    rich: {
      ...parsed.rich,
      nameTable: {
        ...parsed.rich.nameTable,
        names: parsed.rich.nameTable.names.map((name) => (
          name.index === nameIndex ? updater({ ...name }) : name
        )),
      },
    },
  };
}

function cloneAndUpdateRecord(record, updater) {
  return updater({
    ...record,
    uv: record.uv?.map((uv) => ({ ...uv })),
    colors: record.colors?.map((color) => ({ ...color })),
  });
}

function cloneAndUpdateRichItem(item, updater) {
  return updater({
    ...item,
    origin: item.origin ? { ...item.origin } : item.origin,
    localOffset: item.localOffset ? { ...item.localOffset } : item.localOffset,
    transform: item.transform ? { ...item.transform } : item.transform,
    rect: item.rect ? { ...item.rect } : item.rect,
    baseColor: item.baseColor ? { ...item.baseColor, raw: [...item.baseColor.raw] } : item.baseColor,
    meshes: item.meshes,
    attachments: item.attachments,
    controls: item.controls,
    frames: item.frames,
    textureSlots: item.textureSlots ? [...item.textureSlots] : item.textureSlots,
    positions: item.positions?.map((position) => ({ ...position })),
    uv: item.uv?.map((uv) => ({ ...uv })),
    colors: item.colors?.map((color) => ({ ...color, raw: [...color.raw] })),
  });
}

function addRecordToSection(parsed, sectionIndex, type, forcedIndex) {
  if (!parsed) return parsed;
  return {
    ...parsed,
    sections: parsed.sections.map((section) => {
      if (section.index !== sectionIndex) return section;
      if (type === 'draw') {
        const record = makeNewDrawRecord(section.drawRecords, section.index, forcedIndex);
        return {
          ...section,
          header: { ...section.header, drawCount: section.drawRecords.length + 1 },
          drawRecords: [...section.drawRecords, record],
        };
      }
      if (type === 'helper') {
        const record = makeNewHelperRecord(section.helperRecords, section.index, forcedIndex);
        return {
          ...section,
          header: { ...section.header, helperCount: section.helperRecords.length + 1 },
          helperRecords: [...section.helperRecords, record],
        };
      }
      return section;
    }),
  };
}

function addRichRecordToSubresource(parsed, subresourceIndex, type) {
  if (!parsed?.rich) return { parsed, newKey: null };
  let newKey = null;
  const nextParsed = updateRichSubresource(parsed, subresourceIndex, (subresource) => {
    if (type === 'rich-mesh') {
      const index = nextIndex(subresource.meshes);
      newKey = `rich-mesh:${subresourceIndex}:${index}`;
      const mesh = makeNewRichMesh(subresource.meshes, index);
      return {
        ...subresource,
        meshCount: subresource.meshes.length + 1,
        subrecordCount: subresource.meshes.length + 1,
        meshes: [...subresource.meshes, mesh],
        subrecords: [...subresource.meshes, mesh],
      };
    }
    if (type === 'rich-attachment') {
      const index = nextIndex(subresource.attachments);
      newKey = `rich-attachment:${subresourceIndex}:${index}`;
      return {
        ...subresource,
        attachmentCount: subresource.attachments.length + 1,
        field01: subresource.attachments.length + 1,
        attachments: [...subresource.attachments, makeNewRichAttachment(subresource.attachments, index)],
      };
    }
    if (type === 'rich-control') {
      const index = nextIndex(subresource.controls);
      newKey = `rich-control:${subresourceIndex}:${index}`;
      return {
        ...subresource,
        controlCount: subresource.controls.length + 1,
        field02: subresource.controls.length + 1,
        controls: [...subresource.controls, makeNewRichControl(subresource.controls, index)],
      };
    }
    return subresource;
  });
  return { parsed: nextParsed, newKey };
}

function makeNewDrawRecord(records, sectionIndex, forcedIndex) {
  const index = forcedIndex ?? nextIndex(records);
  const source = records[records.length - 1];
  const baseX = source ? source.rawX + 0x40 : 0;
  const baseY = source ? source.rawY + 0x40 : 0;
  return {
    index,
    offset: null,
    isNew: true,
    recordType: source?.recordType ?? 0,
    textureSlot: source?.textureSlot ?? 0,
    recordId: index,
    rawX: baseX,
    rawY: baseY,
    rawDepth: source?.rawDepth ?? 0,
    rawWidth: source?.rawWidth || 0x400,
    rawHeight: source?.rawHeight || 0x200,
    x: baseX * RECT_SCALE,
    y: baseY * RECT_SCALE,
    z: -(source?.rawDepth ?? 0),
    width: (source?.rawWidth || 0x400) * RECT_SCALE,
    height: (source?.rawHeight || 0x200) * RECT_SCALE,
    uv: [
      { rawU: 0, rawV: 0, u: 0, v: 0 },
      { rawU: 4096, rawV: 0, u: 1, v: 0 },
      { rawU: 0, rawV: 4096, u: 0, v: 1 },
      { rawU: 4096, rawV: 4096, u: 1, v: 1 },
    ],
    colors: cloneColors(source?.colors) ?? defaultColors(),
  };
}

function makeNewHelperRecord(records, sectionIndex, forcedIndex) {
  const index = forcedIndex ?? nextIndex(records);
  const source = records[records.length - 1];
  const baseX = source ? source.rawX + 0x40 : 0;
  const baseY = source ? source.rawY + 0x40 : 0;
  return {
    index,
    offset: null,
    isNew: true,
    helperType: source?.helperType ?? 0,
    bankOrSlot: source?.bankOrSlot ?? 0,
    helperId: index,
    rawX: baseX,
    rawY: baseY,
    rawDepth: source?.rawDepth ?? 0,
    rawWidth: source?.rawWidth || 0x400,
    rawHeight: source?.rawHeight || 0x200,
    x: baseX * RECT_SCALE,
    y: baseY * RECT_SCALE,
    z: -(source?.rawDepth ?? 0),
    width: (source?.rawWidth || 0x400) * RECT_SCALE,
    height: (source?.rawHeight || 0x200) * RECT_SCALE,
  };
}

function makeNewRichMesh(records, index) {
  const source = records[records.length - 1];
  const baseX = source ? source.localOffset.rawX + 0x40 : 0;
  const baseY = source ? source.localOffset.rawY + 0x40 : 0;
  const width = source ? Math.max(0x80, richMeshRawBounds(source).rawWidth) : 0x400;
  const height = source ? Math.max(0x80, richMeshRawBounds(source).rawHeight) : 0x200;
  return {
    index,
    offset: null,
    rawMeshPtr: 0,
    valid: true,
    isNew: true,
    flags00: source?.flags00 ?? 0,
    stripCullFlag: source?.stripCullFlag ?? 0,
    hasPtr18: 1,
    textureSlot: source?.textureSlot ?? 0,
    vertexCount: 4,
    baseColor: cloneRichColor(source?.baseColor) ?? makeRichColor(0x7f, 0x7f, 0x7f, 0x7f),
    localOffset: makeVec3(baseX, baseY, source?.localOffset?.rawZ ?? 0),
    rawPositionsPtr: 0,
    positionsOffset: null,
    rawUvPtr: 0,
    uvOffset: null,
    rawColorsPtr: 0,
    colorsOffset: null,
    positions: [
      makeVec3(0, 0, 0),
      makeVec3(width, 0, 0),
      makeVec3(0, height, 0),
      makeVec3(width, height, 0),
    ],
    uv: [
      { rawU: 0, rawV: 0, u: 0, v: 0 },
      { rawU: 4096, rawV: 0, u: 1, v: 0 },
      { rawU: 0, rawV: 4096, u: 0, v: 1 },
      { rawU: 4096, rawV: 4096, u: 1, v: 1 },
    ],
    colors: Array.from({ length: 4 }, () => cloneRichColor(source?.baseColor) ?? makeRichColor(0x7f, 0x7f, 0x7f, 0x7f)),
  };
}

function makeNewRichAttachment(records, index) {
  const source = records[records.length - 1];
  return {
    index,
    offset: null,
    valid: true,
    isNew: true,
    type: source?.type ?? 0,
    textureSlot: source?.textureSlot ?? 0,
    flags02: source?.flags02 ?? 0,
    flags03: source?.flags03 ?? 0,
    transform: makeVec4((source?.transform?.rawX ?? 0) + 0x40, (source?.transform?.rawY ?? 0) + 0x40, source?.transform?.rawZ || 0x400, source?.transform?.rawW || 0x200),
    field0c: source?.field0c ?? 0,
    mode0e: source?.mode0e ?? 0,
    paletteIndex: source?.paletteIndex ?? 0,
    attachmentId: index,
    field12: source?.field12 ?? 0,
    lookupKey: source?.lookupKey ?? 0,
  };
}

function makeNewRichControl(records, index) {
  const source = records[records.length - 1];
  return {
    index,
    offset: null,
    valid: true,
    isNew: true,
    type: source?.type ?? 0,
    field01: source?.field01 ?? 0,
    controlId: index,
    rect: makeRect((source?.rect?.rawX ?? 0) + 0x40, (source?.rect?.rawY ?? 0) + 0x40, source?.rect?.rawDepth ?? 0, source?.rect?.rawWidth || 0x400, source?.rect?.rawHeight || 0x200),
  };
}

function richMeshRawBounds(mesh) {
  const xs = mesh.positions.map((position) => position.rawX);
  const ys = mesh.positions.map((position) => position.rawY);
  return {
    rawX: Math.min(...xs, 0),
    rawY: Math.min(...ys, 0),
    rawWidth: Math.max(...xs, 0) - Math.min(...xs, 0),
    rawHeight: Math.max(...ys, 0) - Math.min(...ys, 0),
  };
}

function makeVec3(rawX, rawY, rawZ) {
  return { rawX, rawY, rawZ, x: rawX * RECT_SCALE, y: rawY * RECT_SCALE, z: rawZ };
}

function makeVec4(rawX, rawY, rawZ, rawW) {
  return { rawX, rawY, rawZ, rawW, x: rawX * RECT_SCALE, y: rawY * RECT_SCALE, z: rawZ, w: rawW };
}

function makeRect(rawX, rawY, rawDepth, rawWidth, rawHeight) {
  return {
    rawX,
    rawY,
    rawDepth,
    rawWidth,
    rawHeight,
    x: rawX * RECT_SCALE,
    y: rawY * RECT_SCALE,
    z: -rawDepth,
    width: rawWidth * RECT_SCALE,
    height: rawHeight * RECT_SCALE,
  };
}

function makeRichColor(r, g, b, a) {
  return { raw: [r, g, b, a], r: expandRichColorByte(r), g: expandRichColorByte(g), b: expandRichColorByte(b), a: expandRichColorByte(a) };
}

function cloneRichColor(color) {
  return color ? { ...color, raw: [...color.raw] } : null;
}

function nextIndex(records) {
  return records.reduce((max, record) => Math.max(max, record.index), -1) + 1;
}

function cloneColors(colors) {
  return colors?.map((color) => ({ ...color }));
}

function defaultColors() {
  return Array.from({ length: 4 }, () => ({ r: 255, g: 255, b: 255, a: 255 }));
}

function makeSaveSignature(parsed) {
  return JSON.stringify({
    textures: parsed.textures.map((texture) => ({
      index: texture.index,
      rawNamePtr: texture.rawNamePtr,
      nameOffset: texture.nameOffset,
      name: texture.name,
    })),
    sections: parsed.sections,
    rich: parsed.rich,
  });
}

function writePatchedBin(parsed) {
  if (parsed.sourceBuffer && parsed.originalSaveSignature === makeSaveSignature(parsed)) {
    return parsed.sourceBuffer.slice(0);
  }

  if (parsed.rich) {
    if (needsRichCanonicalRebuild(parsed)) return writeCanonicalRichBin(parsed);
    const output = parsed.sourceBuffer.slice(0);
    const view = new DataView(output);
    const appended = [];
    writeRichRoot(view, parsed, appended, output.byteLength);
    if (!appended.length) return output;
    const merged = new Uint8Array(output.byteLength + appended.length);
    merged.set(new Uint8Array(output), 0);
    merged.set(appended, output.byteLength);
    return merged.buffer;
  }
  if (!parsed.rich && needsSimpleCanonicalRebuild(parsed)) {
    return writeCanonicalSimpleBin(parsed);
  }

  const output = parsed.sourceBuffer.slice(0);
  const view = new DataView(output);
  const appended = [];

  parsed.sections.forEach((section) => {
    const header = { ...section.header };
    const canPatchHeader = canWrite(view, section.offset, 0x14);

    if (needsTableRebuild(section.drawRecords)) {
      if (canPatchHeader) {
        alignAppended(appended, output.byteLength);
        header.rawDrawPtr = output.byteLength + appended.length;
        header.drawCount = section.drawRecords.length;
        section.drawRecords.forEach((record) => appendDrawRecord(appended, record));
      } else {
        warnSkippedWrite(section, 'draw table rebuild');
        writeExistingDrawRecords(view, section.drawRecords);
      }
    } else {
      writeExistingDrawRecords(view, section.drawRecords);
    }

    if (needsTableRebuild(section.mode1Records)) {
      if (canPatchHeader) {
        alignAppended(appended, output.byteLength);
        header.rawMode1Ptr = output.byteLength + appended.length;
        header.mode1Count = section.mode1Records.length;
        section.mode1Records.forEach((record) => appendMode1Record(appended, record));
      } else {
        warnSkippedWrite(section, 'mode1 table rebuild');
        writeExistingMode1Records(view, section.mode1Records);
      }
    } else {
      writeExistingMode1Records(view, section.mode1Records);
    }

    if (needsTableRebuild(section.helperRecords)) {
      if (canPatchHeader) {
        alignAppended(appended, output.byteLength);
        header.rawHelperPtr = output.byteLength + appended.length;
        header.helperCount = section.helperRecords.length;
        section.helperRecords.forEach((record) => appendHelperRecord(appended, record));
      } else {
        warnSkippedWrite(section, 'helper table rebuild');
        writeExistingHelperRecords(view, section.helperRecords);
      }
    } else {
      writeExistingHelperRecords(view, section.helperRecords);
    }

    if (canPatchHeader) {
      writeSectionHeader(view, { ...section, header });
    } else {
      warnSkippedWrite(section, 'section header');
    }
  });

  if (!appended.length) return output;
  const merged = new Uint8Array(output.byteLength + appended.length);
  merged.set(new Uint8Array(output), 0);
  merged.set(appended, output.byteLength);
  return merged.buffer;
}

function needsTableRebuild(records) {
  return records.some((record) => record.isNew || record.offset === null || record.offset === undefined);
}

function needsSimpleCanonicalRebuild(parsed) {
  return parsed.sections.some((section) => (
    needsTableRebuild(section.drawRecords)
    || needsTableRebuild(section.mode1Records)
    || needsTableRebuild(section.helperRecords)
  ));
}

function needsRichCanonicalRebuild(parsed) {
  const hasNewSubresourceRecord = parsed.rich.subresourceBank.subresources.some((subresource) => (
    subresource.isNew
    || subresource.offset === null
    || subresource.offset === undefined
    || subresource.meshes.some((mesh) => mesh.isNew || mesh.offset === null || mesh.offset === undefined)
    || subresource.attachments.some((attachment) => attachment.isNew || attachment.offset === null || attachment.offset === undefined)
    || subresource.controls.some((control) => control.isNew || control.offset === null || control.offset === undefined)
  ));
  if (hasNewSubresourceRecord) return true;

  const hasNewAssetRecord = parsed.rich.assetBank.assets.some((asset) => (
    asset.isNew
    || asset.offset === null
    || asset.offset === undefined
    || asset.frames.some((frame) => frame.offset === null || frame.offset === undefined)
    || asset.textureSlots.length !== asset.textureSlotCount
  ));
  if (hasNewAssetRecord) return true;

  if (!parsed.sourceBuffer) return true;
  const view = new DataView(parsed.sourceBuffer);
  return parsed.rich.nameTable.names.some((name) => {
    if (name.nameOffset === null || name.nameOffset === undefined) return true;
    return encodeLatin1(name.name ?? '').length > cStringCapacity(view, name.nameOffset);
  });
}

function writeCanonicalRichBin(parsed) {
  const bytes = [];
  const richCache = { arrays: new Map(), meshes: new Map() };
  for (let i = 0; i < 4; i += 1) pushU32(bytes, 0);

  const textureDescOffset = bytes.length;
  patchU32(bytes, 0, pointerForOffset(parsed, textureDescOffset));
  const textureNameTableOffset = textureDescOffset + 0x08;
  const textureNameSlots = [];
  pushU32(bytes, parsed.textures.length);
  pushU32(bytes, pointerForOffset(parsed, textureNameTableOffset));
  parsed.textures.forEach(() => {
    textureNameSlots.push(bytes.length);
    pushU32(bytes, 0);
  });
  parsed.textures.forEach((texture, index) => {
    const offset = bytes.length;
    patchU32(bytes, textureNameSlots[index], pointerForOffset(parsed, offset));
    pushCString(bytes, texture.name ?? '');
  });
  alignBytes(bytes, 4);

  const subresourceTableOffset = bytes.length;
  const subresourceSlots = parsed.rich.subresourceBank.subresources.map(() => {
    const slot = bytes.length;
    pushU32(bytes, 0);
    return slot;
  });

  parsed.rich.subresourceBank.subresources.forEach((subresource, subresourceIndex) => {
    alignBytes(bytes, 4);
    const offset = bytes.length;
    patchU32(bytes, subresourceSlots[subresourceIndex], pointerForOffset(parsed, offset));
    const headerOffset = bytes.length;
    for (let i = 0; i < 0x18; i += 1) bytes.push(0);

    alignBytes(bytes, 4);
    const meshTableOffset = subresource.meshes.length ? bytes.length : 0;
    const meshSlots = subresource.meshes.map(() => {
      const slot = bytes.length;
      pushU32(bytes, 0);
      return slot;
    });

    // Original OPDs place the mesh pointer table before the subresource's
    // mesh bodies, then the non-mesh banks. Keep that shape on growth saves
    // instead of appending new tables at EOF.
    subresource.meshes.forEach((mesh, meshIndex) => {
      const meshOffset = appendSharedRichMesh(bytes, mesh, parsed, richCache);
      patchU32(bytes, meshSlots[meshIndex], pointerForOffset(parsed, meshOffset));
    });

    alignBytes(bytes, 4);
    const attachmentsOffset = subresource.attachments.length ? bytes.length : 0;
    subresource.attachments.forEach((attachment) => appendRichAttachment(bytes, attachment));

    alignBytes(bytes, 4);
    const controlsOffset = subresource.controls.length ? bytes.length : 0;
    subresource.controls.forEach((control) => appendRichControl(bytes, control));

    patchRichSubresourceHeader(bytes, headerOffset, subresource, meshTableOffset, attachmentsOffset, controlsOffset, parsed);
  });

  alignBytes(bytes, 4);
  const subresourceBankOffset = bytes.length;
  patchU32(bytes, 4, pointerForOffset(parsed, subresourceBankOffset));
  pushU32(bytes, parsed.rich.subresourceBank.subresources.length);
  pushU32(bytes, pointerForOffset(parsed, subresourceTableOffset));

  alignBytes(bytes, 4);
  const assetBankOffset = bytes.length;
  patchU32(bytes, 8, pointerForOffset(parsed, assetBankOffset));
  const assetTableOffset = assetBankOffset + 0x08;
  pushU32(bytes, parsed.rich.assetBank.assets.length);
  pushU32(bytes, pointerForOffset(parsed, assetTableOffset));
  const assetSlots = parsed.rich.assetBank.assets.map(() => {
    const slot = bytes.length;
    pushU32(bytes, 0);
    return slot;
  });

  parsed.rich.assetBank.assets.forEach((asset, assetIndex) => {
    alignBytes(bytes, 4);
    const offset = bytes.length;
    patchU32(bytes, assetSlots[assetIndex], pointerForOffset(parsed, offset));
    const headerOffset = bytes.length;
    for (let i = 0; i < 0x0c; i += 1) bytes.push(0);

    alignBytes(bytes, 4);
    const framesOffset = asset.frames.length ? bytes.length : 0;
    asset.frames.forEach((frame) => {
      pushU16(bytes, frame.subresourceIndex);
      pushU16(bytes, frame.frameFlags);
    });

    alignBytes(bytes, 4);
    const textureSlotsOffset = asset.textureSlots.length ? bytes.length : 0;
    asset.textureSlots.forEach((slot) => pushU16(bytes, slot));

    patchRichAssetHeader(bytes, headerOffset, asset, framesOffset, textureSlotsOffset, parsed);
  });

  alignBytes(bytes, 4);
  const nameTableOffset = bytes.length;
  patchU32(bytes, 12, pointerForOffset(parsed, nameTableOffset));
  const richNamePointerTableOffset = nameTableOffset + 0x08;
  pushU32(bytes, parsed.rich.nameTable.names.length);
  pushU32(bytes, pointerForOffset(parsed, richNamePointerTableOffset));
  const richNameSlots = parsed.rich.nameTable.names.map(() => {
    const slot = bytes.length;
    pushU32(bytes, 0);
    return slot;
  });
  parsed.rich.nameTable.names.forEach((name, index) => {
    const offset = bytes.length;
    patchU32(bytes, richNameSlots[index], pointerForOffset(parsed, offset));
    pushCString(bytes, name.name ?? '');
  });

  return new Uint8Array(bytes).buffer;
}

function writeCanonicalSimpleBin(parsed) {
  const bytes = [];
  const rootWordCount = Math.max(2, Math.floor((parsed.root.textureDescOffset ?? 0x08) / 4));
  const textureDescOffset = rootWordCount * 4;
  const rootOffsets = Array.from({ length: rootWordCount }, () => 0);
  rootOffsets[0] = textureDescOffset;
  rootOffsets.forEach(() => pushU32(bytes, 0));

  const textureCount = parsed.textures.length;
  const textureNameTableOffset = textureDescOffset + 0x08;
  const textureStringOffsetSlots = [];
  pushU32(bytes, textureCount);
  pushU32(bytes, pointerForOffset(parsed, textureNameTableOffset));
  for (let i = 0; i < textureCount; i += 1) {
    textureStringOffsetSlots.push(bytes.length);
    pushU32(bytes, 0);
  }

  parsed.textures.forEach((texture, index) => {
    const offset = bytes.length;
    patchU32(bytes, textureStringOffsetSlots[index], pointerForOffset(parsed, offset));
    pushCString(bytes, texture.name ?? '');
  });
  alignBytes(bytes, 4);

  const sectionDirOffset = bytes.length;
  rootOffsets[1] = sectionDirOffset;
  const sectionPointerTableOffset = sectionDirOffset + 0x08;
  pushU32(bytes, parsed.sections.length);
  pushU32(bytes, pointerForOffset(parsed, sectionPointerTableOffset));
  const sectionPointerSlots = [];
  parsed.sections.forEach(() => {
    sectionPointerSlots.push(bytes.length);
    pushU32(bytes, 0);
  });

  const sectionLayouts = parsed.sections.map((section, index) => {
    const offset = bytes.length;
    patchU32(bytes, sectionPointerSlots[index], pointerForOffset(parsed, offset));
    appendCanonicalSectionHeaderPlaceholder(bytes, section);
    return { section, offset, headerPatchOffset: offset };
  });

  sectionLayouts.forEach((layout) => {
    const { section, headerPatchOffset } = layout;
    const header = { ...section.header };

    alignBytes(bytes, 4);
    header.drawCount = section.drawRecords.length;
    header.rawDrawPtr = header.drawCount ? pointerForOffset(parsed, bytes.length) : 0;
    section.drawRecords.forEach((record) => appendDrawRecord(bytes, record));

    alignBytes(bytes, 4);
    header.mode1Count = section.mode1Records.length;
    header.rawMode1Ptr = header.mode1Count ? pointerForOffset(parsed, bytes.length) : 0;
    section.mode1Records.forEach((record) => appendMode1Record(bytes, record));

    alignBytes(bytes, 4);
    header.helperCount = section.helperRecords.length;
    header.rawHelperPtr = header.helperCount ? pointerForOffset(parsed, bytes.length) : 0;
    section.helperRecords.forEach((record) => appendHelperRecord(bytes, record));

    patchCanonicalSectionHeader(bytes, headerPatchOffset, header);
  });

  patchU32(bytes, 0, pointerForOffset(parsed, textureDescOffset));
  for (let i = 1; i < rootOffsets.length; i += 1) {
    patchU32(bytes, i * 4, rootOffsets[i] ? pointerForOffset(parsed, rootOffsets[i]) : 0);
  }

  return new Uint8Array(bytes).buffer;
}

function appendCanonicalSectionHeaderPlaceholder(bytes, section) {
  const header = { ...section.header };
  pushU8(bytes, header.drawCount);
  pushU8(bytes, header.mode1Count);
  pushU8(bytes, header.helperCount);
  pushU8(bytes, header.countFlags03);
  pushU32(bytes, header.rawDrawPtr);
  pushU32(bytes, header.rawMode1Ptr);
  pushU32(bytes, header.rawHelperPtr);
  pushU32(bytes, header.flags10);
}

function patchCanonicalSectionHeader(bytes, offset, header) {
  bytes[offset] = clampNumber(header.drawCount, 0, 255);
  bytes[offset + 0x01] = clampNumber(header.mode1Count, 0, 255);
  bytes[offset + 0x02] = clampNumber(header.helperCount, 0, 255);
  bytes[offset + 0x03] = clampNumber(header.countFlags03, 0, 255);
  patchU32(bytes, offset + 0x04, header.rawDrawPtr);
  patchU32(bytes, offset + 0x08, header.rawMode1Ptr);
  patchU32(bytes, offset + 0x0c, header.rawHelperPtr);
  patchU32(bytes, offset + 0x10, header.flags10);
}

function patchRichSubresourceHeader(bytes, offset, subresource, meshTableOffset, attachmentsOffset, controlsOffset, parsed) {
  bytes[offset] = clampNumber(subresource.meshes.length, 0, 255);
  bytes[offset + 0x01] = clampNumber(subresource.attachments.length, 0, 255);
  bytes[offset + 0x02] = clampNumber(subresource.controls.length, 0, 255);
  bytes[offset + 0x03] = clampNumber(subresource.field03, 0, 255);
  patchVec4(bytes, offset + 0x04, subresource.origin);
  patchU32(bytes, offset + 0x0c, meshTableOffset ? pointerForOffset(parsed, meshTableOffset) : 0);
  patchU32(bytes, offset + 0x10, attachmentsOffset ? pointerForOffset(parsed, attachmentsOffset) : 0);
  patchU32(bytes, offset + 0x14, controlsOffset ? pointerForOffset(parsed, controlsOffset) : 0);
}

function appendSharedRichMesh(bytes, mesh, parsed, cache) {
  const positionBytes = serializeRichPositions(mesh.positions);
  const uvBytes = serializeRichUvs(mesh.uv);
  const colorBytes = shouldWriteRichMeshColors(mesh) ? serializeRichColors(mesh.colors) : [];
  const contentKey = [
    mesh.flags00,
    mesh.stripCullFlag,
    mesh.hasPtr18,
    mesh.textureSlot,
    mesh.positions.length || mesh.vertexCount,
    byteKey(serializeRichColors([mesh.baseColor])),
    byteKey(serializeVec3(mesh.localOffset)),
    byteKey(positionBytes),
    byteKey(uvBytes),
    byteKey(colorBytes),
  ].join('|');
  const key = mesh.offset !== null && mesh.offset !== undefined && !mesh.isNew
    ? `orig:${mesh.offset}:${contentKey}`
    : `new:${contentKey}`;

  const existing = cache.meshes.get(key);
  if (existing !== undefined) return existing;

  alignBytes(bytes, 4);
  const meshOffset = bytes.length;
  const meshHeaderOffset = bytes.length;
  for (let i = 0; i < 0x1c; i += 1) bytes.push(0);

  const positionsOffset = appendSharedRichBlob(bytes, cache, sharedRichBlobKey('pos', mesh.positionsOffset, positionBytes), positionBytes);
  const uvOffset = appendSharedRichBlob(bytes, cache, sharedRichBlobKey('uv', mesh.uvOffset, uvBytes), uvBytes);
  const colorsOffset = appendSharedRichBlob(bytes, cache, sharedRichBlobKey('col', mesh.colorsOffset, colorBytes), colorBytes);

  patchRichMeshHeader(bytes, meshHeaderOffset, mesh, positionsOffset, uvOffset, colorsOffset, parsed);
  cache.meshes.set(key, meshOffset);
  return meshOffset;
}

function sharedRichBlobKey(type, originalOffset, blob) {
  const contentKey = byteKey(blob);
  return originalOffset !== null && originalOffset !== undefined
    ? `${type}:orig:${originalOffset}:${contentKey}`
    : `${type}:new:${contentKey}`;
}

function appendSharedRichBlob(bytes, cache, key, blob) {
  if (!blob.length) return 0;
  const existing = cache.arrays.get(key);
  if (existing !== undefined) return existing;

  alignBytes(bytes, 4);
  const offset = bytes.length;
  blob.forEach((byte) => bytes.push(byte));
  cache.arrays.set(key, offset);
  return offset;
}

function shouldWriteRichMeshColors(mesh) {
  return Boolean(mesh.hasPtr18) && mesh.colors.length > 0;
}

function serializeRichPositions(positions) {
  const bytes = [];
  positions.forEach((position) => appendVec3(bytes, position));
  return bytes;
}

function serializeRichUvs(uvs) {
  const bytes = [];
  uvs.forEach((uv) => {
    pushI16(bytes, uv.rawU);
    pushI16(bytes, uv.rawV);
  });
  return bytes;
}

function serializeRichColors(colors) {
  const bytes = [];
  colors.forEach((color) => appendRichColor(bytes, color));
  return bytes;
}

function serializeVec3(vec) {
  const bytes = [];
  appendVec3(bytes, vec);
  return bytes;
}

function byteKey(bytes) {
  return bytes.join(',');
}

function patchRichMeshHeader(bytes, offset, mesh, positionsOffset, uvOffset, colorsOffset, parsed) {
  bytes[offset] = clampNumber(mesh.flags00, 0, 255);
  bytes[offset + 0x01] = clampNumber(mesh.stripCullFlag, 0, 255);
  bytes[offset + 0x02] = clampNumber(mesh.hasPtr18, 0, 255);
  bytes[offset + 0x03] = clampNumber(mesh.textureSlot, 0, 255);
  patchU16(bytes, offset + 0x04, mesh.positions.length || mesh.vertexCount);
  patchRichColor(bytes, offset + 0x06, mesh.baseColor);
  patchVec3(bytes, offset + 0x0a, mesh.localOffset);
  patchU32(bytes, offset + 0x10, positionsOffset ? pointerForOffset(parsed, positionsOffset) : 0);
  patchU32(bytes, offset + 0x14, uvOffset ? pointerForOffset(parsed, uvOffset) : 0);
  patchU32(bytes, offset + 0x18, colorsOffset ? pointerForOffset(parsed, colorsOffset) : 0);
}

function patchRichAssetHeader(bytes, offset, asset, framesOffset, textureSlotsOffset, parsed) {
  patchU16(bytes, offset, asset.frames.length || asset.frameCount);
  bytes[offset + 0x02] = clampNumber(asset.field02, 0, 255);
  bytes[offset + 0x03] = clampNumber(asset.textureSlots.length || asset.textureSlotCount, 0, 255);
  patchU32(bytes, offset + 0x04, framesOffset ? pointerForOffset(parsed, framesOffset) : 0);
  patchU32(bytes, offset + 0x08, textureSlotsOffset ? pointerForOffset(parsed, textureSlotsOffset) : 0);
}

function appendRichAttachment(bytes, attachment) {
  pushU8(bytes, attachment.type);
  pushU8(bytes, attachment.textureSlot);
  pushU8(bytes, attachment.flags02);
  pushU8(bytes, attachment.flags03);
  appendVec4(bytes, attachment.transform);
  pushU16(bytes, attachment.field0c);
  pushU8(bytes, attachment.mode0e);
  pushU8(bytes, attachment.paletteIndex);
  pushU16(bytes, attachment.attachmentId);
  pushU16(bytes, attachment.field12);
  pushU32(bytes, attachment.lookupKey);
}

function appendRichControl(bytes, control) {
  pushU8(bytes, control.type);
  pushU8(bytes, control.field01);
  pushU16(bytes, control.controlId);
  appendRect(bytes, control.rect);
}

function appendVec3(bytes, vec) {
  pushI16(bytes, vec?.rawX ?? 0);
  pushI16(bytes, vec?.rawY ?? 0);
  pushI16(bytes, vec?.rawZ ?? 0);
}

function appendVec4(bytes, vec) {
  pushI16(bytes, vec?.rawX ?? 0);
  pushI16(bytes, vec?.rawY ?? 0);
  pushI16(bytes, vec?.rawZ ?? 0);
  pushI16(bytes, vec?.rawW ?? 0);
}

function appendRichColor(bytes, color) {
  const raw = color?.raw ?? [0, 0, 0, 0];
  raw.forEach((value) => pushU8(bytes, value));
}

function patchVec3(bytes, offset, vec) {
  patchI16(bytes, offset, vec?.rawX ?? 0);
  patchI16(bytes, offset + 0x02, vec?.rawY ?? 0);
  patchI16(bytes, offset + 0x04, vec?.rawZ ?? 0);
}

function patchVec4(bytes, offset, vec) {
  patchI16(bytes, offset, vec?.rawX ?? 0);
  patchI16(bytes, offset + 0x02, vec?.rawY ?? 0);
  patchI16(bytes, offset + 0x04, vec?.rawZ ?? 0);
  patchI16(bytes, offset + 0x06, vec?.rawW ?? 0);
}

function patchRichColor(bytes, offset, color) {
  const raw = color?.raw ?? [0, 0, 0, 0];
  raw.forEach((value, index) => {
    bytes[offset + index] = clampNumber(value, 0, 255);
  });
}

function alignBytes(bytes, alignment) {
  while (bytes.length % alignment !== 0) bytes.push(0);
}

function pushCString(bytes, value) {
  encodeLatin1(value).forEach((byte) => bytes.push(byte));
  bytes.push(0);
}

function pushU32(bytes, value) {
  const safe = clampNumber(value, 0, 0xffffffff);
  bytes.push(safe & 0xff, (safe >>> 8) & 0xff, (safe >>> 16) & 0xff, (safe >>> 24) & 0xff);
}

function patchU32(bytes, offset, value) {
  const safe = clampNumber(value, 0, 0xffffffff);
  bytes[offset] = safe & 0xff;
  bytes[offset + 0x01] = (safe >>> 8) & 0xff;
  bytes[offset + 0x02] = (safe >>> 16) & 0xff;
  bytes[offset + 0x03] = (safe >>> 24) & 0xff;
}

function patchU16(bytes, offset, value) {
  const safe = clampNumber(value, 0, 65535);
  bytes[offset] = safe & 0xff;
  bytes[offset + 0x01] = (safe >>> 8) & 0xff;
}

function patchI16(bytes, offset, value) {
  const safe = clampNumber(value, -32768, 32767) & 0xffff;
  bytes[offset] = safe & 0xff;
  bytes[offset + 0x01] = (safe >>> 8) & 0xff;
}

function alignAppended(bytes, baseLength) {
  while ((baseLength + bytes.length) % 4 !== 0) bytes.push(0);
}

function canWrite(view, offset, length) {
  return Number.isInteger(offset) && offset >= 0 && offset + length <= view.byteLength;
}

function warnSkippedWrite(item, label) {
  console.warn(
    `[UI BIN Viewer] Skipped ${label}: offset ${fmt(item?.offset)} is outside the loaded file.`,
    item,
  );
}

function writeExistingDrawRecords(view, records) {
  records.forEach((record) => {
    if (!canWrite(view, record.offset, 0x2e)) {
      warnSkippedWrite(record, 'draw record');
      return;
    }
    writeRecordBase(view, record, record.textureSlot);
    record.uv.forEach((uv, index) => {
      view.setInt16(record.offset + 0x0e + index * 4, clampNumber(uv.rawU, -32768, 32767), true);
      view.setInt16(record.offset + 0x10 + index * 4, clampNumber(uv.rawV, -32768, 32767), true);
    });
    writeColors(view, record.offset + 0x1e, record.colors);
  });
}

function writeExistingMode1Records(view, records) {
  records.forEach((record) => {
    if (!canWrite(view, record.offset, 0x1e)) {
      warnSkippedWrite(record, 'mode1 record');
      return;
    }
    writeRecordBase(view, record, record.bankOrSlot);
    writeColors(view, record.offset + 0x0e, record.colors);
  });
}

function writeExistingHelperRecords(view, records) {
  records.forEach((record) => {
    if (!canWrite(view, record.offset, 0x0e)) {
      warnSkippedWrite(record, 'helper record');
      return;
    }
    view.setUint8(record.offset, clampNumber(record.helperType, 0, 255));
    view.setUint8(record.offset + 0x01, clampNumber(record.bankOrSlot, 0, 255));
    view.setUint16(record.offset + 0x02, clampNumber(record.helperId, 0, 65535), true);
    writeRect(view, record);
  });
}

function writeRichRoot(view, parsed, appended, baseLength) {
  const { subresourceBank, assetBank, nameTable } = parsed.rich;
  writeRichBankDesc(view, subresourceBank, 'rich subresource bank');
  writeRichBankDesc(view, assetBank, 'rich asset bank');
  writeRichBankDesc(view, nameTable, 'rich name table');

  subresourceBank.subresources.forEach((subresource) => {
    writeRichSubresource(view, subresource);
    subresource.meshes.forEach((mesh) => writeRichMesh(view, mesh));
    subresource.attachments.forEach((attachment) => writeRichAttachment(view, attachment));
    subresource.controls.forEach((control) => writeRichControl(view, control));
  });

  assetBank.assets.forEach((asset) => writeRichAsset(view, asset));
  writeRichNames(view, parsed, appended, baseLength);
}

function writeRichBankDesc(view, bank, label) {
  if (!canWrite(view, bank.offset, 0x08)) {
    warnSkippedWrite(bank, label);
    return;
  }
  view.setUint32(bank.offset, clampNumber(bank.count, 0, 0xffffffff), true);
  view.setUint32(bank.offset + 0x04, clampNumber(bank.rawTablePtr, 0, 0xffffffff), true);
}

function writeRichSubresource(view, subresource) {
  if (!canWrite(view, subresource.offset, 0x18)) {
    warnSkippedWrite(subresource, 'rich subresource');
    return;
  }
  view.setUint8(subresource.offset, clampNumber(subresource.meshCount, 0, 255));
  view.setUint8(subresource.offset + 0x01, clampNumber(subresource.attachmentCount, 0, 255));
  view.setUint8(subresource.offset + 0x02, clampNumber(subresource.controlCount, 0, 255));
  view.setUint8(subresource.offset + 0x03, clampNumber(subresource.field03, 0, 255));
  writeVec4(view, subresource.offset + 0x04, subresource.origin);
  view.setUint32(subresource.offset + 0x0c, clampNumber(subresource.rawMeshTablePtr, 0, 0xffffffff), true);
  view.setUint32(subresource.offset + 0x10, clampNumber(subresource.rawAttachmentsPtr, 0, 0xffffffff), true);
  view.setUint32(subresource.offset + 0x14, clampNumber(subresource.rawControlsPtr, 0, 0xffffffff), true);
}

function writeRichMesh(view, mesh) {
  if (!canWrite(view, mesh.offset, 0x1c)) {
    warnSkippedWrite(mesh, 'rich mesh');
    return;
  }
  view.setUint8(mesh.offset, clampNumber(mesh.flags00, 0, 255));
  view.setUint8(mesh.offset + 0x01, clampNumber(mesh.stripCullFlag, 0, 255));
  view.setUint8(mesh.offset + 0x02, clampNumber(mesh.hasPtr18, 0, 255));
  view.setUint8(mesh.offset + 0x03, clampNumber(mesh.textureSlot, 0, 255));
  view.setUint16(mesh.offset + 0x04, clampNumber(mesh.vertexCount, 0, 65535), true);
  writeRichColorRaw(view, mesh.offset + 0x06, mesh.baseColor);
  writeVec3(view, mesh.offset + 0x0a, mesh.localOffset);
  view.setUint32(mesh.offset + 0x10, clampNumber(mesh.rawPositionsPtr, 0, 0xffffffff), true);
  view.setUint32(mesh.offset + 0x14, clampNumber(mesh.rawUvPtr, 0, 0xffffffff), true);
  view.setUint32(mesh.offset + 0x18, clampNumber(mesh.rawColorsPtr, 0, 0xffffffff), true);

  if (mesh.positionsOffset !== null) {
    mesh.positions.forEach((position, index) => writeVec3(view, mesh.positionsOffset + index * 6, position));
  }
  if (mesh.uvOffset !== null) {
    mesh.uv.forEach((uv, index) => {
      const offset = mesh.uvOffset + index * 4;
      if (!canWrite(view, offset, 4)) {
        console.warn(`[UI BIN Viewer] Skipped rich UV: offset ${fmt(offset)} is outside the loaded file.`, uv);
        return;
      }
      view.setInt16(offset, clampNumber(uv.rawU, -32768, 32767), true);
      view.setInt16(offset + 0x02, clampNumber(uv.rawV, -32768, 32767), true);
    });
  }
  if (mesh.hasPtr18 && mesh.colorsOffset !== null) {
    mesh.colors.forEach((color, index) => writeRichColorRaw(view, mesh.colorsOffset + index * 4, color));
  }
}

function writeRichAttachment(view, attachment) {
  if (!canWrite(view, attachment.offset, 0x18)) {
    warnSkippedWrite(attachment, 'rich attachment');
    return;
  }
  view.setUint8(attachment.offset, clampNumber(attachment.type, 0, 255));
  view.setUint8(attachment.offset + 0x01, clampNumber(attachment.textureSlot, 0, 255));
  view.setUint8(attachment.offset + 0x02, clampNumber(attachment.flags02, 0, 255));
  view.setUint8(attachment.offset + 0x03, clampNumber(attachment.flags03, 0, 255));
  writeVec4(view, attachment.offset + 0x04, attachment.transform);
  view.setUint16(attachment.offset + 0x0c, clampNumber(attachment.field0c, 0, 65535), true);
  view.setUint8(attachment.offset + 0x0e, clampNumber(attachment.mode0e, 0, 255));
  view.setUint8(attachment.offset + 0x0f, clampNumber(attachment.paletteIndex, 0, 255));
  view.setUint16(attachment.offset + 0x10, clampNumber(attachment.attachmentId, 0, 65535), true);
  view.setUint16(attachment.offset + 0x12, clampNumber(attachment.field12, 0, 65535), true);
  view.setUint32(attachment.offset + 0x14, clampNumber(attachment.lookupKey, 0, 0xffffffff), true);
}

function writeRichControl(view, control) {
  if (!canWrite(view, control.offset, 0x0e)) {
    warnSkippedWrite(control, 'rich control');
    return;
  }
  view.setUint8(control.offset, clampNumber(control.type, 0, 255));
  view.setUint8(control.offset + 0x01, clampNumber(control.field01, 0, 255));
  view.setUint16(control.offset + 0x02, clampNumber(control.controlId, 0, 65535), true);
  writeRectAt(view, control.offset + 0x04, control.rect, control, 'rich control rect');
}

function writeRichAsset(view, asset) {
  if (!canWrite(view, asset.offset, 0x0c)) {
    warnSkippedWrite(asset, 'rich asset');
    return;
  }
  view.setUint16(asset.offset, clampNumber(asset.frameCount, 0, 65535), true);
  view.setUint8(asset.offset + 0x02, clampNumber(asset.field02, 0, 255));
  view.setUint8(asset.offset + 0x03, clampNumber(asset.textureSlotCount, 0, 255));
  view.setUint32(asset.offset + 0x04, clampNumber(asset.rawFramesPtr, 0, 0xffffffff), true);
  view.setUint32(asset.offset + 0x08, clampNumber(asset.rawTextureSlotsPtr, 0, 0xffffffff), true);

  asset.frames.forEach((frame) => {
    if (!canWrite(view, frame.offset, 4)) {
      warnSkippedWrite(frame, 'rich asset frame');
      return;
    }
    view.setUint16(frame.offset, clampNumber(frame.subresourceIndex, 0, 65535), true);
    view.setUint16(frame.offset + 0x02, clampNumber(frame.frameFlags, 0, 65535), true);
  });

  if (asset.textureSlotsOffset !== null) {
    asset.textureSlots.forEach((slot, index) => {
      const offset = asset.textureSlotsOffset + index * 2;
      if (!canWrite(view, offset, 2)) {
        console.warn(`[UI BIN Viewer] Skipped rich texture slot: offset ${fmt(offset)} is outside the loaded file.`, slot);
        return;
      }
      view.setUint16(offset, clampNumber(slot, 0, 65535), true);
    });
  }
}

function writeRichNames(view, parsed, appended, baseLength) {
  const { nameTable } = parsed.rich;
  nameTable.names.forEach((name) => {
    const pointerOffset = nameTable.tableOffset + name.index * 4;
    if (canWrite(view, pointerOffset, 4)) {
      view.setUint32(pointerOffset, clampNumber(name.rawNamePtr, 0, 0xffffffff), true);
    }

    if (name.nameOffset === null) return;
    const bytes = encodeLatin1(name.name ?? '');
    const capacity = cStringCapacity(view, name.nameOffset);
    if (capacity >= bytes.length) {
      writeCStringInPlace(view, name.nameOffset, bytes, capacity);
      return;
    }

    alignAppended(appended, baseLength);
    const newOffset = baseLength + appended.length;
    bytes.forEach((byte) => appended.push(byte));
    appended.push(0);
    if (canWrite(view, pointerOffset, 4)) {
      view.setUint32(pointerOffset, pointerForOffset(parsed, newOffset), true);
    }
  });
}

function writeVec3(view, offset, vec) {
  if (!canWrite(view, offset, 6)) {
    console.warn(`[UI BIN Viewer] Skipped vec3: offset ${fmt(offset)} is outside the loaded file.`, vec);
    return;
  }
  view.setInt16(offset, clampNumber(vec?.rawX, -32768, 32767), true);
  view.setInt16(offset + 0x02, clampNumber(vec?.rawY, -32768, 32767), true);
  view.setInt16(offset + 0x04, clampNumber(vec?.rawZ, -32768, 32767), true);
}

function writeVec4(view, offset, vec) {
  if (!canWrite(view, offset, 8)) {
    console.warn(`[UI BIN Viewer] Skipped vec4: offset ${fmt(offset)} is outside the loaded file.`, vec);
    return;
  }
  view.setInt16(offset, clampNumber(vec?.rawX, -32768, 32767), true);
  view.setInt16(offset + 0x02, clampNumber(vec?.rawY, -32768, 32767), true);
  view.setInt16(offset + 0x04, clampNumber(vec?.rawZ, -32768, 32767), true);
  view.setInt16(offset + 0x06, clampNumber(vec?.rawW, -32768, 32767), true);
}

function writeRectAt(view, offset, rect, item, label) {
  if (!canWrite(view, offset, 0x0a)) {
    warnSkippedWrite(item, label);
    return;
  }
  view.setInt16(offset, clampNumber(rect.rawX, -32768, 32767), true);
  view.setInt16(offset + 0x02, clampNumber(rect.rawY, -32768, 32767), true);
  view.setInt16(offset + 0x04, clampNumber(rect.rawDepth, -32768, 32767), true);
  view.setInt16(offset + 0x06, clampNumber(rect.rawWidth, -32768, 32767), true);
  view.setInt16(offset + 0x08, clampNumber(rect.rawHeight, -32768, 32767), true);
}

function writeRichColorRaw(view, offset, color) {
  if (!canWrite(view, offset, 4)) {
    console.warn(`[UI BIN Viewer] Skipped rich color: offset ${fmt(offset)} is outside the loaded file.`, color);
    return;
  }
  const raw = color?.raw ?? [0, 0, 0, 0];
  raw.forEach((value, index) => view.setUint8(offset + index, clampNumber(value, 0, 255)));
}

function encodeLatin1(value) {
  return Array.from(value, (char) => char.charCodeAt(0) & 0xff);
}

function cStringCapacity(view, offset) {
  if (!canWrite(view, offset, 1)) return -1;
  let length = 0;
  for (let i = offset; i < view.byteLength; i += 1) {
    if (view.getUint8(i) === 0) return length;
    length += 1;
  }
  return -1;
}

function writeCStringInPlace(view, offset, bytes, capacity) {
  bytes.forEach((byte, index) => view.setUint8(offset + index, byte));
  view.setUint8(offset + bytes.length, 0);
  for (let i = bytes.length + 1; i <= capacity; i += 1) {
    view.setUint8(offset + i, 0);
  }
}

function pointerForOffset(parsed, offset) {
  return clampNumber(offset + (parsed.pointerBase || 0), 0, 0xffffffff);
}

function appendDrawRecord(bytes, record) {
  appendRecordBase(bytes, record, record.textureSlot);
  record.uv.forEach((uv) => {
    pushI16(bytes, uv.rawU);
    pushI16(bytes, uv.rawV);
  });
  appendColors(bytes, record.colors);
}

function appendMode1Record(bytes, record) {
  appendRecordBase(bytes, record, record.bankOrSlot);
  appendColors(bytes, record.colors);
}

function appendHelperRecord(bytes, record) {
  pushU8(bytes, record.helperType);
  pushU8(bytes, record.bankOrSlot);
  pushU16(bytes, record.helperId);
  appendRect(bytes, record);
}

function appendRecordBase(bytes, record, slotValue) {
  pushU8(bytes, record.recordType);
  pushU8(bytes, slotValue);
  pushU16(bytes, record.recordId);
  appendRect(bytes, record);
}

function appendRect(bytes, record) {
  pushI16(bytes, record.rawX);
  pushI16(bytes, record.rawY);
  pushI16(bytes, record.rawDepth);
  pushI16(bytes, record.rawWidth);
  pushI16(bytes, record.rawHeight);
}

function appendColors(bytes, colors) {
  colors.forEach((color) => {
    pushU8(bytes, color.r);
    pushU8(bytes, color.g);
    pushU8(bytes, color.b);
    pushU8(bytes, color.a);
  });
}

function pushU8(bytes, value) {
  bytes.push(clampNumber(value, 0, 255));
}

function pushU16(bytes, value) {
  const safe = clampNumber(value, 0, 65535);
  bytes.push(safe & 0xff, (safe >> 8) & 0xff);
}

function pushI16(bytes, value) {
  const safe = clampNumber(value, -32768, 32767) & 0xffff;
  bytes.push(safe & 0xff, (safe >> 8) & 0xff);
}

function writeSectionHeader(view, section) {
  if (!canWrite(view, section.offset, 0x14)) {
    warnSkippedWrite(section, 'section header');
    return;
  }
  view.setUint8(section.offset, clampNumber(section.header.drawCount, 0, 255));
  view.setUint8(section.offset + 0x01, clampNumber(section.header.mode1Count, 0, 255));
  view.setUint8(section.offset + 0x02, clampNumber(section.header.helperCount, 0, 255));
  view.setUint8(section.offset + 0x03, clampNumber(section.header.countFlags03, 0, 255));
  view.setUint32(section.offset + 0x04, clampNumber(section.header.rawDrawPtr, 0, 0xffffffff), true);
  view.setUint32(section.offset + 0x08, clampNumber(section.header.rawMode1Ptr, 0, 0xffffffff), true);
  view.setUint32(section.offset + 0x0c, clampNumber(section.header.rawHelperPtr, 0, 0xffffffff), true);
  view.setUint32(section.offset + 0x10, clampNumber(section.header.flags10, 0, 0xffffffff), true);
}

function writeRecordBase(view, record, slotValue) {
  if (!canWrite(view, record.offset, 0x0e)) {
    warnSkippedWrite(record, 'record base');
    return;
  }
  view.setUint8(record.offset, clampNumber(record.recordType, 0, 255));
  view.setUint8(record.offset + 0x01, clampNumber(slotValue, 0, 255));
  view.setUint16(record.offset + 0x02, clampNumber(record.recordId, 0, 65535), true);
  writeRect(view, record);
}

function writeRect(view, record) {
  if (!canWrite(view, record.offset + 0x04, 0x0a)) {
    warnSkippedWrite(record, 'record rect');
    return;
  }
  view.setInt16(record.offset + 0x04, clampNumber(record.rawX, -32768, 32767), true);
  view.setInt16(record.offset + 0x06, clampNumber(record.rawY, -32768, 32767), true);
  view.setInt16(record.offset + 0x08, clampNumber(record.rawDepth, -32768, 32767), true);
  view.setInt16(record.offset + 0x0a, clampNumber(record.rawWidth, -32768, 32767), true);
  view.setInt16(record.offset + 0x0c, clampNumber(record.rawHeight, -32768, 32767), true);
}

function writeColors(view, offset, colors) {
  if (!canWrite(view, offset, colors.length * 4)) {
    console.warn(`[UI BIN Viewer] Skipped color block: offset ${fmt(offset)} is outside the loaded file.`, colors);
    return;
  }
  colors.forEach((color, index) => {
    view.setUint8(offset + index * 4, clampNumber(color.r, 0, 255));
    view.setUint8(offset + 1 + index * 4, clampNumber(color.g, 0, 255));
    view.setUint8(offset + 2 + index * 4, clampNumber(color.b, 0, 255));
    view.setUint8(offset + 3 + index * 4, clampNumber(color.a, 0, 255));
  });
}

function replaceUv(record, index, u, v) {
  const rawU = Math.round(u / UV_SCALE);
  const rawV = Math.round(v / UV_SCALE);
  return {
    ...record,
    uv: record.uv.map((item, itemIndex) => (
      itemIndex === index ? { rawU, rawV, u: rawU * UV_SCALE, v: rawV * UV_SCALE } : item
    )),
  };
}

function replaceRichUv(record, index, u, v) {
  const rawU = Math.round(u / UV_SCALE);
  const rawV = Math.round(v / UV_SCALE);
  return replaceArrayUv(replaceArrayUv(record, index, 'rawU', rawU), index, 'rawV', rawV);
}

function replaceRawUv(record, index, key, value) {
  return {
    ...record,
    uv: record.uv.map((item, itemIndex) => {
      if (itemIndex !== index) return item;
      const next = { ...item, [key]: value };
      next.u = next.rawU * UV_SCALE;
      next.v = next.rawV * UV_SCALE;
      return next;
    }),
  };
}

function replaceColor(record, index, hexColor) {
  const parsedColor = hexToRgb(hexColor);
  return {
    ...record,
    colors: record.colors.map((color, itemIndex) => (
      itemIndex === index ? { ...color, ...parsedColor } : color
    )),
  };
}

function replaceAlpha(record, index, alpha) {
  return {
    ...record,
    colors: record.colors.map((color, itemIndex) => (
      itemIndex === index ? { ...color, a: alpha } : color
    )),
  };
}

function replaceVecValue(vec, key, value) {
  const next = { ...vec, [key]: value };
  if (key === 'rawX') next.x = value * RECT_SCALE;
  if (key === 'rawY') next.y = value * RECT_SCALE;
  if (key === 'rawZ') next.z = value;
  if (key === 'rawW') next.w = value;
  return next;
}

function replaceRichColorByte(color, channelIndex, value) {
  const raw = [...(color.raw ?? [0, 0, 0, 0])];
  raw[channelIndex] = clampNumber(value, 0, 255);
  return {
    ...color,
    raw,
    r: expandRichColorByte(raw[0]),
    g: expandRichColorByte(raw[1]),
    b: expandRichColorByte(raw[2]),
    a: expandRichColorByte(raw[3]),
  };
}

function expandRichColorByte(value) {
  return value < 0x80 ? value * 2 : 0xff;
}

function replaceArrayVec(mesh, arrayName, index, key, value) {
  return {
    ...mesh,
    [arrayName]: mesh[arrayName].map((item, itemIndex) => (
      itemIndex === index ? replaceVecValue(item, key, value) : item
    )),
  };
}

function replaceArrayUv(mesh, index, key, value) {
  return {
    ...mesh,
    uv: mesh.uv.map((item, itemIndex) => {
      if (itemIndex !== index) return item;
      const next = { ...item, [key]: value };
      next.u = next.rawU * UV_SCALE;
      next.v = next.rawV * UV_SCALE;
      return next;
    }),
  };
}

function replaceArrayColorByte(mesh, index, channelIndex, value) {
  return {
    ...mesh,
    colors: mesh.colors.map((color, itemIndex) => (
      itemIndex === index ? replaceRichColorByte(color, channelIndex, value) : color
    )),
  };
}

function updateHeader(section, key, value) {
  return {
    ...section,
    header: {
      ...section.header,
      [key]: value,
    },
  };
}

function getRecordTable(section, type) {
  const tableName = getRecordTableName(type);
  return tableName ? section[tableName] : null;
}

function getRecordTableName(type) {
  if (type === 'draw') return 'drawRecords';
  if (type === 'mode1') return 'mode1Records';
  if (type === 'helper') return 'helperRecords';
  return null;
}

function makeKey(type, sectionIndex, itemIndex) {
  return itemIndex === undefined ? `${type}:${sectionIndex}` : `${type}:${sectionIndex}:${itemIndex}`;
}

function makeGroupKey(sectionIndex, type) {
  return `group:${sectionIndex}:${type}`;
}

function isExpanded(expandedKeys, key) {
  return expandedKeys[key] ?? true;
}

function isHidden(hiddenKeys, key) {
  return hiddenKeys.includes(key);
}

function rgbToHex(color) {
  return `#${[color.r, color.g, color.b].map((value) => clampNumber(value, 0, 255).toString(16).padStart(2, '0')).join('')}`;
}

function hexToRgb(value) {
  const clean = value.replace('#', '');
  return {
    r: parseInt(clean.slice(0, 2), 16),
    g: parseInt(clean.slice(2, 4), 16),
    b: parseInt(clean.slice(4, 6), 16),
  };
}

function clampNumber(value, min = -Infinity, max = Infinity) {
  const safe = Number.isFinite(value) ? Math.round(value) : 0;
  return Math.min(max, Math.max(min, safe));
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function fmt(value) {
  if (value === null || value === undefined) return 'null';
  return `0x${value.toString(16).toUpperCase()}`;
}

createRoot(document.getElementById('root')).render(<App />);
