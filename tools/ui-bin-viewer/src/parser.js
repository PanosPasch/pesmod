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

const U32 = 4;
const SECTION_HEADER_SIZE = 0x14;
const DRAW_RECORD_SIZE = 0x2e;
const MODE1_RECORD_SIZE = 0x1e;
const HELPER_RECORD_SIZE = 0x0e;
const RECT_SCALE = 1 / 16;
const UV_SCALE = 1 / 4096;
const RICH_ATTACHMENT_RECORD_SIZE = 0x18;
const RICH_CONTROL_RECORD_SIZE = 0x0e;

export function parseUiBin(arrayBuffer, fileName = 'unnamed.bin') {
  const view = new DataView(arrayBuffer);
  const byteLength = view.byteLength;
  const warnings = [];

  if (byteLength < U32) {
    throw new Error('File is too small to contain a UI BIN root pointer.');
  }

  const pointerBase = inferPointerBase(view);
  const resolve = (ptr) => resolvePointer(ptr, byteLength, pointerBase);
  const rawTexturePtr = u32(view, 0);
  const textureDescOffset = resolve(rawTexturePtr);

  if (textureDescOffset === null) {
    throw new Error(`Root texture descriptor pointer ${hex(rawTexturePtr)} cannot be resolved.`);
  }

  const rootWordCount = Math.max(1, Math.floor(textureDescOffset / U32));
  const extraRoots = [];
  for (let i = 1; i < rootWordCount; i += 1) {
    const raw = u32(view, i * U32);
    extraRoots.push(resolve(raw) ?? raw);
  }

  const textures = parseTextures(view, textureDescOffset, resolve, warnings);
  const richRoot = detectRichRoot(view, extraRoots, resolve, warnings);
  const sectionDir = richRoot ? null : findSectionDirectory(view, extraRoots, resolve, warnings);
  const sections = sectionDir
    ? sectionDir.sectionOffsets.map((offset, index) => parseSection(view, offset, index, resolve, warnings))
    : [];

  const result = {
    fileName,
    byteLength,
    pointerBase,
    root: {
      rawTexturePtr,
      textureDescOffset,
      extraRoots,
    },
    layoutKind: richRoot ? 'rich' : 'simple',
    rich: richRoot,
    textures,
    sectionDir,
    sections,
    warnings,
  };

  if (!sectionDir && !richRoot) {
    warnings.push('No plausible section directory was found in the extra root pointers.');
  }

  return result;
}

function parseTextures(view, offset, resolve, warnings) {
  if (!canRead(view, offset, U32 * 2)) {
    warnings.push(`Texture descriptor ${hex(offset)} is outside the file.`);
    return [];
  }

  const count = u32(view, offset);
  const rawNameTablePtr = u32(view, offset + U32);
  const nameTableOffset = resolve(rawNameTablePtr);
  if (count > 4096) {
    warnings.push(`Texture count ${count} at ${hex(offset)} looks implausibly high; clamping to 4096.`);
  }
  if (nameTableOffset === null && count > 0) {
    warnings.push(`Texture name table pointer ${hex(rawNameTablePtr)} cannot be resolved.`);
    return [];
  }

  const safeCount = Math.min(count, 4096);
  const textures = [];
  for (let i = 0; i < safeCount; i += 1) {
    const ptrOffset = nameTableOffset + i * U32;
    if (!canRead(view, ptrOffset, U32)) {
      warnings.push(`Texture pointer ${i} at ${hex(ptrOffset)} is outside the file.`);
      break;
    }

    const rawNamePtr = u32(view, ptrOffset);
    const nameOffset = resolve(rawNamePtr);
    const name = nameOffset === null ? '' : readCString(view, nameOffset, warnings);
    if (nameOffset === null) {
      warnings.push(`Texture name pointer ${i} (${hex(rawNamePtr)}) could not be resolved.`);
    }

    textures.push({ index: i, rawNamePtr, nameOffset, name });
  }
  textures.nameTableOffset = nameTableOffset;
  textures.rawNameTablePtr = rawNameTablePtr;
  return textures;
}

function detectRichRoot(view, extraRoots, resolve, warnings) {
  if (extraRoots.length < 3) return null;

  const [subresourceBankOffset, assetBankOffset, nameTableOffset] = extraRoots;
  if (!looksLikePointerListDesc(view, subresourceBankOffset, resolve, 1, 1024)) return null;
  if (!looksLikePointerListDesc(view, assetBankOffset, resolve, 0, 1024)) return null;
  if (!looksLikeNameTableDesc(view, nameTableOffset, resolve)) return null;

  const subresourceBank = parseRichSubresourceBank(view, subresourceBankOffset, resolve, warnings);
  const assetBank = parseRichAssetBank(view, assetBankOffset, resolve, warnings);

  return {
    subresourceBank,
    assetBank,
    nameTable: parseRichNameTable(view, nameTableOffset, resolve, warnings),
    // Back-compat aliases for older UI panels/tools.
    elementBank: { ...subresourceBank, elements: subresourceBank.subresources },
    groupBank: { ...assetBank, groups: assetBank.assets },
  };
}

function looksLikePointerListDesc(view, offset, resolve, minCount, maxCount) {
  if (!canRead(view, offset, U32 * 2)) return false;

  const count = u32(view, offset);
  const tableOffset = resolve(u32(view, offset + U32));
  if (count === 0) return minCount === 0;
  return count >= minCount
    && count <= maxCount
    && tableOffset !== null
    && canRead(view, tableOffset, count * U32);
}

function looksLikeNameTableDesc(view, offset, resolve) {
  if (!looksLikePointerListDesc(view, offset, resolve, 0, 4096)) return false;

  const count = u32(view, offset);
  const tableOffset = resolve(u32(view, offset + U32));
  for (let i = 0; i < Math.min(count, 8); i += 1) {
    const nameOffset = resolve(u32(view, tableOffset + i * U32));
    if (nameOffset === null || !canRead(view, nameOffset, 1)) return false;
  }
  return true;
}

function parseRichSubresourceBank(view, offset, resolve, warnings) {
  const count = u32(view, offset);
  const rawTablePtr = u32(view, offset + U32);
  const tableOffset = resolve(rawTablePtr);
  const subresources = [];

  for (let i = 0; i < count; i += 1) {
    const pointerOffset = tableOffset + i * U32;
    const rawSubresourcePtr = u32(view, pointerOffset);
    const subresourceOffset = resolve(rawSubresourcePtr);
    subresources.push(parseRichSubresource(view, subresourceOffset, i, rawSubresourcePtr, resolve, warnings));
  }

  return {
    offset,
    count,
    rawTablePtr,
    tableOffset,
    subresources,
  };
}

function parseRichSubresource(view, offset, index, rawSubresourcePtr, resolve, warnings) {
  const fallback = {
    index,
    offset,
    rawSubresourcePtr,
    valid: false,
    meshes: [],
    attachments: [],
    controls: [],
    warnings: [],
  };

  if (!canRead(view, offset, 0x18)) {
    fallback.warnings.push(`Rich subresource record ${hex(offset)} is outside the file.`);
    warnings.push(`Rich subresource ${index}: record ${hex(offset)} is outside the file.`);
    return fallback;
  }

  const meshCount = view.getUint8(offset);
  const attachmentCount = view.getUint8(offset + 0x01);
  const controlCount = view.getUint8(offset + 0x02);
  const rawMeshTablePtr = u32(view, offset + 0x0c);
  const meshTableOffset = resolve(rawMeshTablePtr);
  const rawAttachmentsPtr = u32(view, offset + 0x10);
  const attachmentsOffset = resolve(rawAttachmentsPtr);
  const rawControlsPtr = u32(view, offset + 0x14);
  const controlsOffset = resolve(rawControlsPtr);
  const subresourceWarnings = [];
  const meshes = [];
  const attachments = [];
  const controls = [];

  if (meshCount && meshTableOffset === null) {
    subresourceWarnings.push(`Mesh pointer table ${hex(rawMeshTablePtr)} cannot be resolved.`);
  } else if (meshTableOffset !== null) {
    for (let i = 0; i < meshCount; i += 1) {
      const pointerOffset = meshTableOffset + i * U32;
      if (!canRead(view, pointerOffset, U32)) {
        subresourceWarnings.push(`Mesh pointer ${i} at ${hex(pointerOffset)} is outside the file.`);
        break;
      }
      const rawMeshPtr = u32(view, pointerOffset);
      const meshOffset = resolve(rawMeshPtr);
      meshes.push(parseRichMesh(view, meshOffset, i, rawMeshPtr, resolve, warnings));
    }
  }

  if (attachmentCount && attachmentsOffset === null) {
    subresourceWarnings.push(`Attachment table ${hex(rawAttachmentsPtr)} cannot be resolved.`);
  } else if (attachmentsOffset !== null) {
    for (let i = 0; i < attachmentCount; i += 1) {
      attachments.push(parseRichAttachmentRecord(view, attachmentsOffset + i * RICH_ATTACHMENT_RECORD_SIZE, i, warnings));
    }
  }

  if (controlCount && controlsOffset === null) {
    subresourceWarnings.push(`Control table ${hex(rawControlsPtr)} cannot be resolved.`);
  } else if (controlsOffset !== null) {
    for (let i = 0; i < controlCount; i += 1) {
      controls.push(parseRichControlRecord(view, controlsOffset + i * RICH_CONTROL_RECORD_SIZE, i, warnings));
    }
  }

  for (const line of subresourceWarnings) warnings.push(`Rich subresource ${index}: ${line}`);

  return {
    index,
    offset,
    rawSubresourcePtr,
    valid: true,
    meshCount,
    attachmentCount,
    controlCount,
    field03: view.getUint8(offset + 0x03),
    field04: u32(view, offset + 0x04),
    field08: u32(view, offset + 0x08),
    origin: parseRichVec4(view, offset + 0x04),
    rawMeshTablePtr,
    meshTableOffset,
    rawAttachmentsPtr,
    attachmentsOffset,
    rawControlsPtr,
    controlsOffset,
    meshes,
    attachments,
    controls,
    // Back-compat aliases for older UI panels/tools.
    subrecordCount: meshCount,
    subrecords: meshes,
    field01: attachmentCount,
    field02: controlCount,
    rawSubrecordsPtr: rawMeshTablePtr,
    subrecordsOffset: meshTableOffset,
    rawPtr10: rawAttachmentsPtr,
    ptr10Offset: attachmentsOffset,
    rawPtr14: rawControlsPtr,
    ptr14Offset: controlsOffset,
    warnings: subresourceWarnings,
  };
}

function parseRichMesh(view, offset, index, rawMeshPtr, resolve, warnings) {
  const fallback = {
    index,
    offset,
    rawMeshPtr,
    valid: false,
    positions: [],
    uv: [],
    colors: [],
  };

  if (!canRead(view, offset, 0x1c)) {
    warnings.push(`Rich mesh ${index}: record ${hex(offset)} is outside the file.`);
    return fallback;
  }

  const vertexCount = u16(view, offset + 0x04);
  const rawPositionsPtr = u32(view, offset + 0x10);
  const positionsOffset = resolve(rawPositionsPtr);
  const rawUvPtr = u32(view, offset + 0x14);
  const uvOffset = resolve(rawUvPtr);
  const rawColorsPtr = u32(view, offset + 0x18);
  const colorsOffset = resolve(rawColorsPtr);
  const baseColor = parseRichColor(view, offset + 0x06);
  const positions = positionsOffset === null ? [] : parseRichPositions(view, positionsOffset, vertexCount);
  const uv = uvOffset === null ? [] : parseRichUvs(view, uvOffset, vertexCount);
  const colors = view.getUint8(offset + 0x02) && colorsOffset !== null
    ? parseRichColors(view, colorsOffset, vertexCount)
    : Array.from({ length: vertexCount }, () => baseColor);

  return {
    index,
    offset,
    rawMeshPtr,
    valid: true,
    flags00: view.getUint8(offset),
    stripCullFlag: view.getUint8(offset + 0x01),
    hasPtr18: view.getUint8(offset + 0x02),
    textureSlot: view.getUint8(offset + 0x03),
    vertexCount,
    rawBaseColor: u32(view, offset + 0x06),
    baseColor,
    localOffset: parseRichVec3(view, offset + 0x0a),
    rawPositionsPtr,
    positionsOffset,
    rawUvPtr,
    uvOffset,
    rawColorsPtr,
    colorsOffset,
    positions,
    uv,
    colors,
    // Back-compat aliases.
    field00: view.getUint8(offset),
    field01: view.getUint8(offset + 0x01),
    field03: view.getUint8(offset + 0x03),
    field04: u32(view, offset + 0x04),
    field08: u32(view, offset + 0x08),
    field0c: u32(view, offset + 0x0c),
    rawPtr10: rawPositionsPtr,
    rawPtr14: rawUvPtr,
    rawPtr18: rawColorsPtr,
  };
}

function parseRichAttachmentRecord(view, offset, index, warnings) {
  if (!canRead(view, offset, RICH_ATTACHMENT_RECORD_SIZE)) {
    warnings.push(`Rich attachment ${index}: record ${hex(offset)} is outside the file.`);
    return { index, offset, valid: false };
  }

  return {
    index,
    offset,
    valid: true,
    type: view.getUint8(offset),
    textureSlot: view.getUint8(offset + 0x01),
    flags02: view.getUint8(offset + 0x02),
    flags03: view.getUint8(offset + 0x03),
    transform: parseRichVec4(view, offset + 0x04),
    field0c: u16(view, offset + 0x0c),
    mode0e: view.getUint8(offset + 0x0e),
    paletteIndex: view.getUint8(offset + 0x0f),
    attachmentId: u16(view, offset + 0x10),
    field12: u16(view, offset + 0x12),
    lookupKey: u32(view, offset + 0x14),
  };
}

function parseRichControlRecord(view, offset, index, warnings) {
  if (!canRead(view, offset, RICH_CONTROL_RECORD_SIZE)) {
    warnings.push(`Rich control ${index}: record ${hex(offset)} is outside the file.`);
    return { index, offset, valid: false };
  }

  return {
    index,
    offset,
    valid: true,
    type: view.getUint8(offset),
    field01: view.getUint8(offset + 0x01),
    controlId: u16(view, offset + 0x02),
    rect: parseRectFields(view, offset),
  };
}

function parseRichAssetBank(view, offset, resolve, warnings) {
  const count = u32(view, offset);
  const rawTablePtr = u32(view, offset + U32);
  const tableOffset = resolve(rawTablePtr);
  const assets = [];

  for (let i = 0; i < count; i += 1) {
    const pointerOffset = tableOffset + i * U32;
    const rawAssetPtr = u32(view, pointerOffset);
    const assetOffset = resolve(rawAssetPtr);
    assets.push(parseRichAssetEntry(view, assetOffset, i, rawAssetPtr, resolve, warnings));
  }

  return {
    offset,
    count,
    rawTablePtr,
    tableOffset,
    assets,
  };
}

function parseRichAssetEntry(view, offset, index, rawAssetPtr, resolve, warnings) {
  const fallback = {
    index,
    offset,
    rawAssetPtr,
    valid: false,
    frames: [],
    textureSlots: [],
  };

  if (!canRead(view, offset, 0x0c)) {
    warnings.push(`Rich asset ${index}: record ${hex(offset)} is outside the file.`);
    return fallback;
  }

  const frameCount = u16(view, offset);
  const rawFramesPtr = u32(view, offset + 0x04);
  const framesOffset = resolve(rawFramesPtr);
  const rawTextureSlotsPtr = u32(view, offset + 0x08);
  const textureSlotsOffset = resolve(rawTextureSlotsPtr);
  const frames = [];
  const textureSlots = [];

  if (framesOffset !== null) {
    for (let i = 0; i < frameCount; i += 1) {
      frames.push({
        index: i,
        offset: framesOffset + i * U32,
        subresourceIndex: u16(view, framesOffset + i * U32),
        frameFlags: u16(view, framesOffset + i * U32 + 0x02),
      });
    }
  }

  if (textureSlotsOffset !== null) {
    for (let i = 0; i < view.getUint8(offset + 0x03); i += 1) {
      textureSlots.push(u16(view, textureSlotsOffset + i * 2));
    }
  }

  return {
    index,
    offset,
    rawAssetPtr,
    valid: true,
    frameCount,
    field02: view.getUint8(offset + 0x02),
    textureSlotCount: view.getUint8(offset + 0x03),
    rawFramesPtr,
    framesOffset,
    rawTextureSlotsPtr,
    textureSlotsOffset,
    frames,
    textureSlots,
    // Back-compat aliases.
    rawGroupPtr: rawAssetPtr,
    packedTypeIdFlags: u32(view, offset),
    type: frameCount,
    id: view.getUint8(offset + 0x02),
    flags: view.getUint8(offset + 0x03),
    rawPtr04: rawFramesPtr,
    ptr04Offset: framesOffset,
    rawPtr08: rawTextureSlotsPtr,
    ptr08Offset: textureSlotsOffset,
  };
}

function parseRichVec4(view, offset) {
  const rawX = i16(view, offset);
  const rawY = i16(view, offset + 0x02);
  const rawZ = i16(view, offset + 0x04);
  const rawW = i16(view, offset + 0x06);
  return {
    rawX,
    rawY,
    rawZ,
    rawW,
    x: rawX * RECT_SCALE,
    y: rawY * RECT_SCALE,
    z: rawZ,
    w: rawW,
  };
}

function parseRichVec3(view, offset) {
  const rawX = i16(view, offset);
  const rawY = i16(view, offset + 0x02);
  const rawZ = i16(view, offset + 0x04);
  return {
    rawX,
    rawY,
    rawZ,
    x: rawX * RECT_SCALE,
    y: rawY * RECT_SCALE,
    z: rawZ,
  };
}

function parseRichPositions(view, offset, count) {
  const positions = [];
  for (let i = 0; i < count; i += 1) {
    if (!canRead(view, offset + i * 6, 6)) break;
    positions.push(parseRichVec3(view, offset + i * 6));
  }
  return positions;
}

function parseRichUvs(view, offset, count) {
  const uv = [];
  for (let i = 0; i < count; i += 1) {
    if (!canRead(view, offset + i * 4, 4)) break;
    const rawU = i16(view, offset + i * 4);
    const rawV = i16(view, offset + i * 4 + 0x02);
    uv.push({
      rawU,
      rawV,
      u: rawU * UV_SCALE,
      v: rawV * UV_SCALE,
    });
  }
  return uv;
}

function parseRichColors(view, offset, count) {
  const colors = [];
  for (let i = 0; i < count; i += 1) {
    if (!canRead(view, offset + i * 4, 4)) break;
    colors.push(parseRichColor(view, offset + i * 4));
  }
  return colors;
}

function parseRichColor(view, offset) {
  const raw = [
    view.getUint8(offset),
    view.getUint8(offset + 0x01),
    view.getUint8(offset + 0x02),
    view.getUint8(offset + 0x03),
  ];
  const expanded = raw.map(expandRichColorByte);
  return {
    raw,
    r: expanded[0],
    g: expanded[1],
    b: expanded[2],
    a: expanded[3],
  };
}

function expandRichColorByte(value) {
  return value < 0x80 ? value * 2 : 0xff;
}

function parseRichNameTable(view, offset, resolve, warnings) {
  const count = u32(view, offset);
  const rawTablePtr = u32(view, offset + U32);
  const tableOffset = resolve(rawTablePtr);
  const names = [];

  for (let i = 0; i < count; i += 1) {
    const pointerOffset = tableOffset + i * U32;
    const rawNamePtr = u32(view, pointerOffset);
    const nameOffset = resolve(rawNamePtr);
    names.push({
      index: i,
      rawNamePtr,
      nameOffset,
      name: nameOffset === null ? '' : readCString(view, nameOffset, warnings),
    });
  }

  return {
    offset,
    count,
    rawTablePtr,
    tableOffset,
    names,
  };
}

function findSectionDirectory(view, extraRoots, resolve, warnings) {
  const candidates = [];

  for (const root of extraRoots) {
    const offset = resolve(root) ?? root;
    if (!Number.isInteger(offset) || !canRead(view, offset, U32 * 2)) continue;

    const count = u32(view, offset);
    if (count < 1 || count > 256) continue;

    const tablePtr = resolve(u32(view, offset + U32));
    if (tablePtr !== null) {
      const indirect = scoreSectionPointerTable(view, tablePtr, count, resolve);
      if (indirect.score > 0) {
        candidates.push({ offset, count, tableOffset: tablePtr, tableShape: 'indirect', ...indirect });
      }
    }

    const inline = scoreSectionPointerTable(view, offset + U32, count, resolve);
    if (inline.score > 0) {
      candidates.push({ offset, count, tableOffset: offset + U32, tableShape: 'inline-fallback', ...inline });
    }
  }

  if (!candidates.length) return null;
  candidates.sort((a, b) => {
    const ratio = b.score / b.count - a.score / a.count;
    const shape = shapePriority(b.tableShape) - shapePriority(a.tableShape);
    return ratio || b.score - a.score || shape || a.count - b.count || a.offset - b.offset;
  });
  const best = candidates[0];

  if (best.score !== best.count) {
    warnings.push(
      `Section directory ${hex(best.offset)} (${best.tableShape}) has ${best.score}/${best.count} plausible section headers.`,
    );
  }

  return {
    offset: best.offset,
    count: best.count,
    score: best.score,
    tableOffset: best.tableOffset,
    tableShape: best.tableShape,
    sectionOffsets: best.sectionOffsets,
    candidates,
  };
}

function shapePriority(shape) {
  return shape === 'indirect' ? 1 : 0;
}

function scoreSectionPointerTable(view, tableOffset, count, resolve) {
  if (!canRead(view, tableOffset, count * U32)) {
    return { score: 0, sectionOffsets: [] };
  }

  const sectionOffsets = [];
  let score = 0;
  for (let i = 0; i < count; i += 1) {
    const raw = u32(view, tableOffset + i * U32);
    const sectionOffset = resolve(raw);
    sectionOffsets.push(sectionOffset ?? raw);
    if (sectionOffset !== null && looksLikeSectionHeader(view, sectionOffset, resolve)) {
      score += 1;
    }
  }

  return { score, sectionOffsets };
}

function parseSection(view, offset, index, resolve, warnings) {
  const fallback = {
    index,
    offset,
    valid: false,
    header: null,
    drawRecords: [],
    mode1Records: [],
    helperRecords: [],
    warnings: [],
  };

  if (!canRead(view, offset, SECTION_HEADER_SIZE)) {
    fallback.warnings.push(`Section header ${hex(offset)} is outside the file.`);
    warnings.push(`Section ${index}: header ${hex(offset)} is outside the file.`);
    return fallback;
  }

  const drawCount = view.getUint8(offset);
  const mode1Count = view.getUint8(offset + 0x01);
  const helperCount = view.getUint8(offset + 0x02);
  const countFlags03 = view.getUint8(offset + 0x03);
  const rawDrawPtr = u32(view, offset + 0x04);
  const rawMode1Ptr = u32(view, offset + 0x08);
  const rawHelperPtr = u32(view, offset + 0x0c);
  const flags10 = u32(view, offset + 0x10);
  const drawOffset = resolve(rawDrawPtr);
  const mode1Offset = resolve(rawMode1Ptr);
  const helperOffset = resolve(rawHelperPtr);
  const sectionWarnings = [];

  const drawRecords = [];
  if (drawOffset === null && drawCount) {
    sectionWarnings.push(`Draw pointer ${hex(rawDrawPtr)} cannot be resolved.`);
  } else if (drawOffset !== null) {
    for (let i = 0; i < drawCount; i += 1) {
      const recordOffset = drawOffset + i * DRAW_RECORD_SIZE;
      if (!canRead(view, recordOffset, DRAW_RECORD_SIZE)) {
        sectionWarnings.push(`Draw record ${i} at ${hex(recordOffset)} is outside the file.`);
        break;
      }
      drawRecords.push(parseDrawRecord(view, recordOffset, i));
    }
  }

  const mode1Records = [];
  if (mode1Offset === null && mode1Count) {
    sectionWarnings.push(`Mode1 pointer ${hex(rawMode1Ptr)} cannot be resolved.`);
  } else if (mode1Offset !== null) {
    for (let i = 0; i < mode1Count; i += 1) {
      const recordOffset = mode1Offset + i * MODE1_RECORD_SIZE;
      if (!canRead(view, recordOffset, MODE1_RECORD_SIZE)) {
        sectionWarnings.push(`Mode1 record ${i} at ${hex(recordOffset)} is outside the file.`);
        break;
      }
      mode1Records.push(parseMode1Record(view, recordOffset, i));
    }
  }

  const helperRecords = [];
  if (helperOffset === null && helperCount) {
    sectionWarnings.push(`Helper pointer ${hex(rawHelperPtr)} cannot be resolved.`);
  } else if (helperOffset !== null) {
    for (let i = 0; i < helperCount; i += 1) {
      const recordOffset = helperOffset + i * HELPER_RECORD_SIZE;
      if (!canRead(view, recordOffset, HELPER_RECORD_SIZE)) {
        sectionWarnings.push(`Helper record ${i} at ${hex(recordOffset)} is outside the file.`);
        break;
      }
      helperRecords.push(parseHelperRecord(view, recordOffset, i));
    }
  }

  for (const line of sectionWarnings) warnings.push(`Section ${index}: ${line}`);

  return {
    index,
    offset,
    valid: true,
    header: {
      helperCount,
      drawCount,
      mode1Count,
      countFlags03,
      rawDrawPtr,
      drawOffset,
      rawMode1Ptr,
      mode1Offset,
      rawHelperPtr,
      helperOffset,
      flags10,
    },
    drawRecords,
    mode1Records,
    helperRecords,
    warnings: sectionWarnings,
  };
}

function parseDrawRecord(view, offset, index) {
  const rect = parseRectFields(view, offset);
  const uv = [];
  for (let i = 0; i < 4; i += 1) {
    const rawU = i16(view, offset + 0x0e + i * 4);
    const rawV = i16(view, offset + 0x10 + i * 4);
    uv.push({
      rawU,
      rawV,
      u: rawU * UV_SCALE,
      v: rawV * UV_SCALE,
    });
  }

  const colors = [];
  for (let i = 0; i < 4; i += 1) {
    colors.push({
      r: view.getUint8(offset + 0x1e + i * 4),
      g: view.getUint8(offset + 0x1f + i * 4),
      b: view.getUint8(offset + 0x20 + i * 4),
      a: view.getUint8(offset + 0x21 + i * 4),
    });
  }

  return {
    index,
    offset,
    recordType: view.getUint8(offset),
    textureSlot: view.getUint8(offset + 0x01),
    recordId: u16(view, offset + 0x02),
    ...rect,
    uv,
    colors,
  };
}

function parseMode1Record(view, offset, index) {
  return {
    index,
    offset,
    recordType: view.getUint8(offset),
    bankOrSlot: view.getUint8(offset + 0x01),
    recordId: u16(view, offset + 0x02),
    ...parseRectFields(view, offset),
    colors: parseColors(view, offset + 0x0e),
  };
}

function parseHelperRecord(view, offset, index) {
  return {
    index,
    offset,
    helperType: view.getUint8(offset),
    bankOrSlot: view.getUint8(offset + 0x01),
    helperId: u16(view, offset + 0x02),
    ...parseRectFields(view, offset),
  };
}

function parseRectFields(view, offset) {
  const rawX = i16(view, offset + 0x04);
  const rawY = i16(view, offset + 0x06);
  const rawDepth = i16(view, offset + 0x08);
  const rawWidth = i16(view, offset + 0x0a);
  const rawHeight = i16(view, offset + 0x0c);
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

function parseColors(view, offset) {
  const colors = [];
  for (let i = 0; i < 4; i += 1) {
    colors.push({
      r: view.getUint8(offset + i * 4),
      g: view.getUint8(offset + 1 + i * 4),
      b: view.getUint8(offset + 2 + i * 4),
      a: view.getUint8(offset + 3 + i * 4),
    });
  }
  return colors;
}

function looksLikeSectionHeader(view, offset, resolve) {
  if (!canRead(view, offset, SECTION_HEADER_SIZE)) return false;

  const drawCount = view.getUint8(offset);
  const mode1Count = view.getUint8(offset + 0x01);
  const helperCount = view.getUint8(offset + 0x02);
  const drawOffset = resolve(u32(view, offset + 0x04));
  const mode1Offset = resolve(u32(view, offset + 0x08));
  const helperOffset = resolve(u32(view, offset + 0x0c));
  if (helperCount > 240 || mode1Count > 240 || drawCount > 240) return false;
  if (drawCount && (drawOffset === null || !canRead(view, drawOffset, drawCount * DRAW_RECORD_SIZE))) {
    return false;
  }
  if (mode1Count && (mode1Offset === null || !canRead(view, mode1Offset, mode1Count * MODE1_RECORD_SIZE))) {
    return false;
  }
  if (helperCount && (helperOffset === null || !canRead(view, helperOffset, helperCount * HELPER_RECORD_SIZE))) {
    return false;
  }
  return true;
}

function inferPointerBase(view) {
  const byteLength = view.byteLength;
  const rootPtr = u32(view, 0);
  if (rootPtr < byteLength) return 0;

  const maxOffset = Math.min(byteLength - U32, 0x20000);
  for (let possibleRootOffset = U32; possibleRootOffset <= maxOffset; possibleRootOffset += U32) {
    const base = rootPtr - possibleRootOffset;
    if (looksLikeTextureDescriptorAt(view, possibleRootOffset, base)) return base;
  }

  let best = { base: 0, score: -1 };

  for (let possibleOffset = 0; possibleOffset <= maxOffset; possibleOffset += U32) {
    const base = rootPtr - possibleOffset;
    let score = 0;
    for (let pos = 0; pos < Math.min(byteLength, 0x400); pos += U32) {
      const ptr = u32(view, pos);
      const resolved = ptr - base;
      if (resolved >= 0 && resolved < byteLength && resolved % U32 === 0) score += 1;
    }
    if (score > best.score) best = { base, score };
  }

  return best.score > 2 ? best.base : 0;
}

function looksLikeTextureDescriptorAt(view, offset, pointerBase) {
  if (!canRead(view, offset, U32 * 2)) return false;

  const count = u32(view, offset);
  if (count < 1 || count > 4096) return false;

  const tableOffset = resolvePointer(u32(view, offset + U32), view.byteLength, pointerBase);
  if (tableOffset === null || !canRead(view, tableOffset, count * U32)) return false;

  let plausibleStrings = 0;
  for (let i = 0; i < Math.min(count, 16); i += 1) {
    const stringOffset = resolvePointer(u32(view, tableOffset + i * U32), view.byteLength, pointerBase);
    if (stringOffset !== null && canRead(view, stringOffset, 1)) plausibleStrings += 1;
  }
  return plausibleStrings > 0;
}

function resolvePointer(ptr, byteLength, pointerBase) {
  if (ptr === 0) return null;
  if (ptr > 0 && ptr < byteLength) return ptr;
  const relocated = ptr - pointerBase;
  if (relocated > 0 && relocated < byteLength) return relocated;
  return null;
}

function readCString(view, offset, warnings) {
  if (!canRead(view, offset, 1)) return '';

  const bytes = [];
  for (let i = offset; i < view.byteLength; i += 1) {
    const value = view.getUint8(i);
    if (value === 0) break;
    if (bytes.length > 512) {
      warnings.push(`String at ${hex(offset)} exceeded 512 bytes; truncating.`);
      break;
    }
    bytes.push(value);
  }

  return new TextDecoder('windows-1252').decode(new Uint8Array(bytes));
}

function canRead(view, offset, length) {
  return Number.isInteger(offset) && offset >= 0 && offset + length <= view.byteLength;
}

function u16(view, offset) {
  return view.getUint16(offset, true);
}

function i16(view, offset) {
  return view.getInt16(offset, true);
}

function u32(view, offset) {
  return view.getUint32(offset, true);
}

function hex(value) {
  if (value === null || value === undefined) return 'null';
  return `0x${value.toString(16).toUpperCase()}`;
}
