import React, { useEffect, useMemo, useRef } from 'react';
import * as THREE from 'three';

const MOVE_SPEED = 520;
const FAST_MULTIPLIER = 3.5;
const MOUSE_SENSITIVITY = 0.0025;
const ORTHO_PADDING = 1.25;
const HANDLE_SIZE = 16;
const RECT_SCALE = 1 / 16;

export function UiBinScene({ parsed, textureFiles, selectedKey, projectionMode, hiddenKeys, onEditItem, onSelect }) {
  const mountRef = useRef(null);
  const parsedRef = useRef(parsed);
  const textureFilesRef = useRef(textureFiles);
  const selectedKeyRef = useRef(selectedKey);
  const projectionModeRef = useRef(projectionMode);
  const hiddenKeysRef = useRef(hiddenKeys);
  const onEditItemRef = useRef(onEditItem);
  const onSelectRef = useRef(onSelect);
  const frameKeyRef = useRef('');
  const sceneStateRef = useRef(null);

  const bounds = useMemo(() => computeBounds(parsed), [parsed]);

  useEffect(() => {
    parsedRef.current = parsed;
    textureFilesRef.current = textureFiles;
    selectedKeyRef.current = selectedKey;
    projectionModeRef.current = projectionMode;
    hiddenKeysRef.current = hiddenKeys;
    onEditItemRef.current = onEditItem;
    onSelectRef.current = onSelect;
    rebuildScene(sceneStateRef.current, parsed, textureFiles, bounds, selectedKey, hiddenKeys);
    const frameKey = parsed ? `${parsed.fileName}:${parsed.byteLength}:${projectionMode}` : `empty:${projectionMode}`;
    if (sceneStateRef.current && frameKeyRef.current !== frameKey) {
      frameCamera(sceneStateRef.current.camera, bounds, sceneStateRef.current.mount, projectionMode);
      frameKeyRef.current = frameKey;
    }
  }, [parsed, textureFiles, bounds, selectedKey, projectionMode, hiddenKeys, onEditItem, onSelect]);

  useEffect(() => {
    const mount = mountRef.current;
    if (!mount) return undefined;

    const renderer = new THREE.WebGLRenderer({ antialias: true, alpha: false });
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setSize(mount.clientWidth, mount.clientHeight);
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    mount.appendChild(renderer.domElement);

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x111318);

    const camera = makeCamera(projectionMode, mount);
    frameCamera(camera, bounds, mount, projectionMode);

    const root = new THREE.Group();
    scene.add(root);

    const grid = new THREE.GridHelper(6000, 24, 0x56616f, 0x2a3038);
    grid.position.y = 380;
    grid.rotation.x = Math.PI / 2;
    scene.add(grid);

    const axes = new THREE.AxesHelper(800);
    scene.add(axes);

    const ambient = new THREE.AmbientLight(0xffffff, 0.72);
    const key = new THREE.DirectionalLight(0xffffff, 1.8);
    key.position.set(600, 700, 900);
    scene.add(ambient, key);

    const keys = new Set();
    const look = { yaw: 0, pitch: 0, dragging: false, lastX: 0, lastY: 0, downX: 0, downY: 0 };
    const clock = new THREE.Clock();
    const labels = [];
    const sceneTextures = [];
    const selectableObjects = [];
    const handleObjects = [];
    const raycaster = new THREE.Raycaster();
    const pointer = new THREE.Vector2();
    const activeTransform = { current: null };

    const state = {
      renderer,
      scene,
      camera,
      root,
      labels,
      sceneTextures,
      selectableObjects,
      handleObjects,
      raycaster,
      pointer,
      activeTransform,
      mount,
    };
    sceneStateRef.current = state;
    frameKeyRef.current = parsedRef.current
      ? `${parsedRef.current.fileName}:${parsedRef.current.byteLength}:${projectionMode}`
      : `empty:${projectionMode}`;
    rebuildScene(state, parsedRef.current, textureFilesRef.current, bounds, selectedKeyRef.current, hiddenKeysRef.current);

    const onResize = () => {
      renderer.setSize(mount.clientWidth, mount.clientHeight);
      updateCameraProjection(camera, mount, computeBounds(parsedRef.current), projectionModeRef.current);
    };

    const onKeyDown = (event) => {
      if (event.code === 'KeyR') {
        frameCamera(camera, computeBounds(parsedRef.current), mount, projectionModeRef.current);
        look.yaw = 0;
        look.pitch = 0;
      }
      keys.add(event.code);
    };
    const onKeyUp = (event) => keys.delete(event.code);

    const onPointerDown = (event) => {
      if (camera.isOrthographicCamera) {
        const transform = pickTransformObject(state, event);
        if (transform) {
          onSelectRef.current?.(transform.key);
          const world = pointerToWorld(state, event);
          activeTransform.current = {
            ...transform,
            lastWorld: world,
          };
          renderer.domElement.setPointerCapture(event.pointerId);
          return;
        }
      }
      look.dragging = true;
      look.lastX = event.clientX;
      look.lastY = event.clientY;
      look.downX = event.clientX;
      look.downY = event.clientY;
      renderer.domElement.setPointerCapture(event.pointerId);
    };
    const onPointerMove = (event) => {
      if (activeTransform.current) {
        const world = pointerToWorld(state, event);
        const dx = world.x - activeTransform.current.lastWorld.x;
        const dy = world.y - activeTransform.current.lastWorld.y;
        activeTransform.current.lastWorld = world;
        applyTransform(activeTransform.current, dx, dy, onEditItemRef.current);
        return;
      }
      if (!look.dragging) return;
      const dx = event.clientX - look.lastX;
      const dy = event.clientY - look.lastY;
      look.lastX = event.clientX;
      look.lastY = event.clientY;
      if (camera.isOrthographicCamera) {
        panOrthographicCamera(camera, dx, dy, mount);
        return;
      }
      look.yaw -= dx * MOUSE_SENSITIVITY;
      look.pitch = clamp(look.pitch - dy * MOUSE_SENSITIVITY, -1.45, 1.45);
      camera.rotation.order = 'YXZ';
      camera.rotation.y = look.yaw;
      camera.rotation.x = look.pitch;
    };
    const onPointerUp = (event) => {
      if (activeTransform.current) {
        activeTransform.current = null;
        if (renderer.domElement.hasPointerCapture(event.pointerId)) {
          renderer.domElement.releasePointerCapture(event.pointerId);
        }
        return;
      }
      look.dragging = false;
      const moved = Math.hypot(event.clientX - look.downX, event.clientY - look.downY);
      if (moved < 4) {
        pickObject(state, event, onSelectRef.current);
      }
      if (renderer.domElement.hasPointerCapture(event.pointerId)) {
        renderer.domElement.releasePointerCapture(event.pointerId);
      }
    };
    const onWheel = (event) => {
      if (camera.isOrthographicCamera) {
        camera.zoom = clamp(camera.zoom * (event.deltaY > 0 ? 0.9 : 1.1), 0.05, 80);
        camera.updateProjectionMatrix();
      } else {
        camera.translateZ(event.deltaY * 0.55);
      }
    };

    window.addEventListener('resize', onResize);
    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('keyup', onKeyUp);
    renderer.domElement.addEventListener('pointerdown', onPointerDown);
    renderer.domElement.addEventListener('pointermove', onPointerMove);
    renderer.domElement.addEventListener('pointerup', onPointerUp);
    renderer.domElement.addEventListener('wheel', onWheel, { passive: true });

    let frameId = 0;
    const animate = () => {
      const delta = clock.getDelta();
      moveCamera(camera, keys, delta);
      updateLabels(labels, camera, mount);
      renderer.render(scene, camera);
      frameId = requestAnimationFrame(animate);
    };
    animate();

    return () => {
      cancelAnimationFrame(frameId);
      window.removeEventListener('resize', onResize);
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('keyup', onKeyUp);
      renderer.domElement.removeEventListener('pointerdown', onPointerDown);
      renderer.domElement.removeEventListener('pointermove', onPointerMove);
      renderer.domElement.removeEventListener('pointerup', onPointerUp);
      renderer.domElement.removeEventListener('wheel', onWheel);
      disposeGroup(root);
      sceneTextures.forEach((texture) => texture.dispose());
      renderer.dispose();
      mount.removeChild(renderer.domElement);
      sceneStateRef.current = null;
    };
  }, [projectionMode]);

  return (
    <div className="scene-wrap" ref={mountRef}>
      {!parsed ? (
        <div className="empty-state">
          <h2>Load a BIN file</h2>
          <p>Parsed sections, draw quads, helper records, texture fallbacks, and labels will appear here.</p>
        </div>
      ) : null}
    </div>
  );
}

function rebuildScene(state, parsed, textureFiles, bounds, selectedKey, hiddenKeys = []) {
  if (!state) return;

  clearChildren(state.root);
  state.labels.splice(0).forEach((label) => label.element.remove());
  state.sceneTextures.splice(0).forEach((texture) => texture.dispose());
  state.selectableObjects.splice(0);
  state.handleObjects.splice(0);

  if (!parsed) return;

  const textureMap = buildTextureMap(parsed, textureFiles, state);
  const centerX = (bounds.minX + bounds.maxX) / 2;
  const centerY = (bounds.minY + bounds.maxY) / 2;
  const width = Math.max(1, bounds.maxX - bounds.minX);
  const height = Math.max(1, bounds.maxY - bounds.minY);
  const sectionSpacing = Math.max(width, height, 700) * 0.18;

  if (!parsed.sections.length && parsed.rich) {
    addRichRootScene(state, parsed, textureMap, centerX, centerY, hiddenKeys, selectedKey);
    return;
  }

  if (!parsed.sections.length) {
    addUnknownRootFallbacks(state, parsed, width, height);
    return;
  }

  parsed.sections.forEach((section, sectionIndex) => {
    const sectionKey = makeTreeKey('section', section.index);
    if (isHidden(hiddenKeys, sectionKey)) return;

    const group = new THREE.Group();
    group.position.z = (sectionIndex - (parsed.sections.length - 1) / 2) * sectionSpacing;
    state.root.add(group);

    const sectionColor = sectionIndex % 2 === 0 ? 0x66d9ef : 0xa6e22e;
    addWireBox(group, width, height, sectionColor, `section ${section.index}`, state.labels);

    if (!isHidden(hiddenKeys, makeGroupKey(section.index, 'draw'))) section.drawRecords.forEach((record) => {
      const texture = textureMap.get(record.textureSlot) ?? textureMap.get(0);
      const key = makeTreeKey('draw', section.index, record.index);
      if (isHidden(hiddenKeys, key)) return;
      const mesh = makeQuad(record, texture, centerX, centerY, key === selectedKey);
      if (mesh) {
        mesh.position.z = depthOffset(record, record.index, 0);
        mesh.userData.label = `S${section.index} draw ${record.recordId} tex ${record.textureSlot}`;
        mesh.userData.selectKey = key;
        group.add(mesh);
        state.selectableObjects.push(mesh);
        if (key === selectedKey) addTransformHandles(group, record, key, centerX, centerY, state.handleObjects, depthOffset(record, record.index, 140));
        mesh.updateWorldMatrix(true, false);
        addLabel(
          state.mount,
          state.labels,
          mesh.userData.label,
          mesh.localToWorld(new THREE.Vector3()),
          'draw-label',
        );
      } else {
        const fallback = makeFallbackBox(record, centerX, centerY);
        fallback.userData.label = `S${section.index} invalid draw ${record.recordId}`;
        fallback.userData.selectKey = key;
        group.add(fallback);
        state.selectableObjects.push(fallback);
      }
    });

    if (!isHidden(hiddenKeys, makeGroupKey(section.index, 'helper'))) section.helperRecords.forEach((helper, index) => {
      const key = makeTreeKey('helper', section.index, helper.index);
      if (isHidden(hiddenKeys, key)) return;
      const marker = makeHelperMarker(helper, index, centerX, centerY, key === selectedKey);
      marker.userData.selectKey = key;
      group.add(marker);
      state.selectableObjects.push(marker);
      if (key === selectedKey) addTransformHandles(group, helper, key, centerX, centerY, state.handleObjects, depthOffset(helper, index, 160));
      marker.updateWorldMatrix(true, false);
      addLabel(
        state.mount,
        state.labels,
        `S${section.index} helper ${helper.helperId}`,
        marker.localToWorld(new THREE.Vector3()),
        'helper-label',
      );
    });
  });
}

function addUnknownRootFallbacks(state, parsed, width, height) {
  const group = new THREE.Group();
  state.root.add(group);

  const material = new THREE.MeshStandardMaterial({ color: 0x6ba8ff, roughness: 0.5 });
  parsed.root.extraRoots.forEach((root, index) => {
    const geometry = new THREE.BoxGeometry(180, 120, 70);
    const mesh = new THREE.Mesh(geometry, material.clone());
    mesh.position.set((index - (parsed.root.extraRoots.length - 1) / 2) * 230, 0, 0);
    group.add(mesh);
    mesh.updateWorldMatrix(true, false);
    addLabel(
      state.mount,
      state.labels,
      `extra root ${index}: ${formatHex(root)}`,
      mesh.localToWorld(new THREE.Vector3(0, 90, 0)),
      'section-label',
    );
  });

  parsed.textures.forEach((texture, index) => {
    const labelTexture = makeLabelTexture(texture.name || `texture_${texture.index}`);
    state.sceneTextures.push(labelTexture);
    const geometry = new THREE.PlaneGeometry(220, 140);
    const mesh = new THREE.Mesh(
      geometry,
      new THREE.MeshBasicMaterial({ map: labelTexture, side: THREE.DoubleSide }),
    );
    mesh.position.set((index - (parsed.textures.length - 1) / 2) * 250, -height * 0.3 - 220, 0);
    group.add(mesh);
  });

  addWireBox(group, Math.max(width, 600), Math.max(height, 400), 0xffd166, 'fallback root view', state.labels);
}

function addRichRootScene(state, parsed, textureMap, centerX, centerY, hiddenKeys, selectedKey) {
  const group = new THREE.Group();
  state.root.add(group);

  if (isHidden(hiddenKeys, 'rich-bank:subresources')) return;

  parsed.rich.subresourceBank.subresources.forEach((subresource) => {
    const subresourceKey = `rich-subresource:${subresource.index}`;
    if (isHidden(hiddenKeys, subresourceKey)) return;

    if (!isHidden(hiddenKeys, `rich-mesh-group:${subresource.index}`)) {
      subresource.meshes.forEach((mesh) => {
        const key = `rich-mesh:${subresource.index}:${mesh.index}`;
        if (isHidden(hiddenKeys, key)) return;

        const texture = textureMap.get(mesh.textureSlot) ?? textureMap.get(0);
        const object = makeRichMesh(subresource, mesh, texture, centerX, centerY, key === selectedKey);
        if (!object) return;

        object.userData.label = `R${subresource.index} mesh ${mesh.index} tex ${mesh.textureSlot}`;
        object.userData.selectKey = key;
        group.add(object);
        state.selectableObjects.push(object);
        const rect = richMeshSpatialRect(subresource, mesh);
        if (key === selectedKey) addTransformHandles(group, rect, key, centerX, centerY, state.handleObjects, depthOffset(rect, mesh.index, 180));
        object.updateWorldMatrix(true, false);
        addLabel(state.mount, state.labels, object.userData.label, object.localToWorld(new THREE.Vector3()), 'draw-label');
      });
    }

    if (!isHidden(hiddenKeys, `rich-attachment-group:${subresource.index}`)) {
      subresource.attachments.forEach((attachment) => {
        const key = `rich-attachment:${subresource.index}:${attachment.index}`;
        if (isHidden(hiddenKeys, key)) return;
        const texture = textureMap.get(attachment.textureSlot) ?? textureMap.get(0);
        const rect = attachmentSpatialRect(subresource, attachment);
        const object = makeSpatialMarker(rect, attachment.index, centerX, centerY, key === selectedKey, 0x8bd3ff, texture);
        object.userData.selectKey = key;
        object.userData.label = `R${subresource.index} attachment ${attachment.index} tex ${attachment.textureSlot}`;
        group.add(object);
        state.selectableObjects.push(object);
        if (key === selectedKey) addTransformHandles(group, rect, key, centerX, centerY, state.handleObjects, depthOffset(rect, attachment.index, 190));
        object.updateWorldMatrix(true, false);
        addLabel(state.mount, state.labels, object.userData.label, object.localToWorld(new THREE.Vector3()), 'helper-label');
      });
    }

    if (!isHidden(hiddenKeys, `rich-control-group:${subresource.index}`)) {
      subresource.controls.forEach((control) => {
        const key = `rich-control:${subresource.index}:${control.index}`;
        if (isHidden(hiddenKeys, key)) return;
        const rect = controlSpatialRect(subresource, control);
        const object = makeSpatialMarker(rect, control.index, centerX, centerY, key === selectedKey, 0xf7d774);
        object.userData.selectKey = key;
        object.userData.label = `R${subresource.index} control ${control.controlId}`;
        group.add(object);
        state.selectableObjects.push(object);
        if (key === selectedKey) addTransformHandles(group, rect, key, centerX, centerY, state.handleObjects, depthOffset(rect, control.index, 200));
        object.updateWorldMatrix(true, false);
        addLabel(state.mount, state.labels, object.userData.label, object.localToWorld(new THREE.Vector3()), 'helper-label');
      });
    }
  });

  addWireBox(group, Math.max(600, computeBounds(parsed).maxX - computeBounds(parsed).minX), Math.max(400, computeBounds(parsed).maxY - computeBounds(parsed).minY), 0xffd166, 'rich OPD mesh view', state.labels);
}

function buildTextureMap(parsed, textureFiles, state) {
  const map = new Map();
  parsed.textures.forEach((texture) => {
    const url = textureFiles[texture.name];
    let threeTexture;
    if (url) {
      threeTexture = new THREE.TextureLoader().load(url);
      threeTexture.colorSpace = THREE.SRGBColorSpace;
      threeTexture.flipY = false;
    } else {
      threeTexture = makeLabelTexture(texture.name || `texture_${texture.index}`);
    }
    state.sceneTextures.push(threeTexture);
    map.set(texture.index, threeTexture);
  });

  if (!map.has(0)) {
    const fallback = makeLabelTexture('no texture descriptor');
    state.sceneTextures.push(fallback);
    map.set(0, fallback);
  }

  return map;
}

function makeQuad(record, texture, centerX, centerY, selected) {
  const x0 = record.x;
  const y0 = record.y;
  const x1 = record.x + record.width;
  const y1 = record.y + record.height;
  const vertices = [
    new THREE.Vector3(x0 - centerX, centerY - y0, 0),
    new THREE.Vector3(x1 - centerX, centerY - y0, 0),
    new THREE.Vector3(x0 - centerX, centerY - y1, 0),
    new THREE.Vector3(x1 - centerX, centerY - y1, 0),
  ];
  const area = polygonArea(vertices);
  if (Math.abs(area) < 0.01) return null;

  const geometry = new THREE.BufferGeometry();
  geometry.setFromPoints(vertices);
  geometry.setIndex([0, 2, 1, 2, 3, 1]);
  geometry.setAttribute(
    'uv',
    new THREE.Float32BufferAttribute(record.uv.flatMap((uv) => [uv.u, uv.v]), 2),
  );
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(record.colors.flatMap(toColorFloat), 3));
  geometry.computeVertexNormals();
  geometry.computeBoundingSphere();

  const opacity = Math.max(0.2, averageAlpha(record.colors));
  return new THREE.Mesh(
    geometry,
    new THREE.MeshBasicMaterial({
      map: texture,
      transparent: opacity < 1,
      opacity,
      side: THREE.DoubleSide,
      vertexColors: true,
      wireframe: selected,
    }),
  );
}

function makeRichMesh(subresource, mesh, texture, centerX, centerY, selected) {
  if (!mesh.positions.length || mesh.positions.length < 3) return null;

  const geometry = new THREE.BufferGeometry();
  const positions = [];
  const colors = [];
  const uvs = [];
  const baseX = (subresource.origin?.x ?? 0) + (mesh.localOffset?.x ?? 0);
  const baseY = (subresource.origin?.y ?? 0) + (mesh.localOffset?.y ?? 0);
  const baseZ = (subresource.origin?.z ?? 0) + (mesh.localOffset?.z ?? 0);

  mesh.positions.forEach((position, index) => {
    positions.push(
      baseX + position.x - centerX,
      centerY - (baseY + position.y),
      (baseZ + position.z) * 0.1 + subresource.index * 0.35,
    );
    const uv = mesh.uv[index] ?? { u: 0, v: 0 };
    uvs.push(uv.u, uv.v);
    colors.push(...toColorFloat(mesh.colors[index] ?? mesh.baseColor));
  });

  geometry.setAttribute('position', new THREE.Float32BufferAttribute(positions, 3));
  geometry.setAttribute('uv', new THREE.Float32BufferAttribute(uvs, 2));
  geometry.setAttribute('color', new THREE.Float32BufferAttribute(colors, 3));
  geometry.setIndex(makeTriangleStripIndices(mesh.positions.length));
  geometry.computeVertexNormals();
  geometry.computeBoundingSphere();

  const opacity = Math.max(0.2, averageAlpha(mesh.colors.length ? mesh.colors : [mesh.baseColor]));
  return new THREE.Mesh(
    geometry,
    new THREE.MeshBasicMaterial({
      map: texture,
      transparent: opacity < 1,
      opacity,
      side: THREE.DoubleSide,
      vertexColors: true,
      wireframe: selected,
    }),
  );
}

function makeSpatialMarker(rect, index, centerX, centerY, selected, color, texture) {
  const width = Math.max(Math.abs(rect.width), 8);
  const height = Math.max(Math.abs(rect.height), 8);
  const x = rect.x + rect.width / 2 - centerX;
  const y = centerY - (rect.y + rect.height / 2);
  const geometry = new THREE.PlaneGeometry(width, height);
  const material = new THREE.MeshBasicMaterial({
    map: texture ?? null,
    color: texture ? 0xffffff : (selected ? 0xfff2a8 : color),
    opacity: selected ? 0.5 : 0.3,
    transparent: true,
    side: THREE.DoubleSide,
    depthTest: true,
    depthWrite: false,
    wireframe: selected && !texture,
  });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.position.set(Number.isFinite(x) ? x : index * 35, Number.isFinite(y) ? y : 0, depthOffset(rect, index, 120));
  const edges = new THREE.EdgesGeometry(geometry);
  const outline = new THREE.LineSegments(
    edges,
    new THREE.LineBasicMaterial({ color: selected ? 0xffffff : color }),
  );
  mesh.add(outline);
  return mesh;
}

function richMeshSpatialRect(subresource, mesh) {
  const baseRawX = subresource.origin?.rawX ?? 0;
  const baseRawY = subresource.origin?.rawY ?? 0;
  const localRawX = mesh.localOffset?.rawX ?? 0;
  const localRawY = mesh.localOffset?.rawY ?? 0;
  const xs = mesh.positions.map((position) => position.rawX);
  const ys = mesh.positions.map((position) => position.rawY);
  const minX = Math.min(...xs, 0);
  const minY = Math.min(...ys, 0);
  const maxX = Math.max(...xs, 1);
  const maxY = Math.max(...ys, 1);
  return makeRawRect(
    baseRawX + localRawX + minX,
    baseRawY + localRawY + minY,
    -(subresource.origin?.rawZ ?? 0) - (mesh.localOffset?.rawZ ?? 0),
    Math.max(1, maxX - minX),
    Math.max(1, maxY - minY),
  );
}

function attachmentSpatialRect(subresource, attachment) {
  return makeRawRect(
    (subresource.origin?.rawX ?? 0) + (attachment.transform?.rawX ?? 0),
    (subresource.origin?.rawY ?? 0) + (attachment.transform?.rawY ?? 0),
    -(attachment.transform?.rawZ ?? 0),
    Math.max(1, Math.abs(attachment.transform?.rawZ ?? 0x100)),
    Math.max(1, Math.abs(attachment.transform?.rawW ?? 0x100)),
  );
}

function controlSpatialRect(subresource, control) {
  const rect = control.rect ?? {};
  return makeRawRect(
    (subresource.origin?.rawX ?? 0) + (rect.rawX ?? 0),
    (subresource.origin?.rawY ?? 0) + (rect.rawY ?? 0),
    rect.rawDepth ?? 0,
    Math.max(1, rect.rawWidth ?? 1),
    Math.max(1, rect.rawHeight ?? 1),
  );
}

function makeRawRect(rawX, rawY, rawDepth, rawWidth, rawHeight) {
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

function makeFallbackBox(record, centerX, centerY) {
  const geometry = new THREE.BoxGeometry(42, 42, 42);
  const material = new THREE.MeshStandardMaterial({ color: 0xff6b6b, roughness: 0.55 });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.position.set(record.x - centerX, centerY - record.y, record.index * 8);
  return mesh;
}

function makeHelperMarker(helper, index, centerX, centerY, selected) {
  const width = Math.max(Math.abs(helper.width), 8);
  const height = Math.max(Math.abs(helper.height), 8);
  const x = helper.x + helper.width / 2 - centerX;
  const y = centerY - (helper.y + helper.height / 2);
  const geometry = new THREE.PlaneGeometry(width, height);
  const material = new THREE.MeshBasicMaterial({
    color: selected ? 0xfff2a8 : 0xf7d774,
    opacity: selected ? 0.42 : 0.24,
    transparent: true,
    side: THREE.DoubleSide,
    depthTest: true,
    depthWrite: false,
  });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.position.set(
    Number.isFinite(x) ? x : index * 35,
    Number.isFinite(y) ? y : 0,
    depthOffset(helper, index, 80),
  );
  const edges = new THREE.EdgesGeometry(geometry);
  const outline = new THREE.LineSegments(
    edges,
    new THREE.LineBasicMaterial({ color: selected ? 0xffffff : 0xf7d774 }),
  );
  mesh.add(outline);
  return mesh;
}

function addTransformHandles(group, record, key, centerX, centerY, handleObjects, z) {
  const x0 = record.x - centerX;
  const y0 = centerY - record.y;
  const x1 = record.x + record.width - centerX;
  const y1 = centerY - (record.y + record.height);
  const cx = (x0 + x1) / 2;
  const cy = (y0 + y1) / 2;
  const positions = [
    ['nw', x0, y0],
    ['n', cx, y0],
    ['ne', x1, y0],
    ['e', x1, cy],
    ['se', x1, y1],
    ['s', cx, y1],
    ['sw', x0, y1],
    ['w', x0, cy],
  ];
  const geometry = new THREE.PlaneGeometry(HANDLE_SIZE, HANDLE_SIZE);
  positions.forEach(([handle, x, y]) => {
    const mesh = new THREE.Mesh(
      geometry.clone(),
      new THREE.MeshBasicMaterial({
        color: isCornerHandle(handle) ? 0xfff2a8 : 0x66d9ef,
        depthTest: false,
        depthWrite: false,
      }),
    );
    mesh.position.set(x, y, z);
    mesh.userData.transform = { key, handle };
    group.add(mesh);
    handleObjects.push(mesh);
  });
}

function addWireBox(group, width, height, color, text, labels) {
  const geometry = new THREE.BoxGeometry(width, height, 2);
  const edges = new THREE.EdgesGeometry(geometry);
  const line = new THREE.LineSegments(edges, new THREE.LineBasicMaterial({ color }));
  group.add(line);
  const labelPosition = new THREE.Vector3(-width / 2, height / 2 + 42, 0);
  group.updateWorldMatrix(true, false);
  labels.push({ element: makeLabelElement(text, 'section-label'), position: group.localToWorld(labelPosition) });
}

function addLabel(mount, labels, text, position, className) {
  const element = makeLabelElement(text, className);
  mount.appendChild(element);
  labels.push({ element, position });
}

function makeLabelElement(text, className) {
  const element = document.createElement('div');
  element.className = `scene-label ${className}`;
  element.textContent = text;
  return element;
}

function updateLabels(labels, camera, mount) {
  const rect = mount.getBoundingClientRect();
  labels.forEach((label) => {
    if (!label.element.parentElement) mount.appendChild(label.element);
    const position = label.position.clone().project(camera);
    const visible = position.z < 1;
    label.element.style.display = visible ? 'block' : 'none';
    label.element.style.transform = `translate(${(position.x * 0.5 + 0.5) * rect.width}px, ${(-position.y * 0.5 + 0.5) * rect.height}px)`;
  });
}

function moveCamera(camera, keys, delta) {
  const speed = MOVE_SPEED * (keys.has('ShiftLeft') || keys.has('ShiftRight') ? FAST_MULTIPLIER : 1) * delta;
  if (camera.isOrthographicCamera) {
    if (keys.has('KeyW')) camera.position.y += speed;
    if (keys.has('KeyS')) camera.position.y -= speed;
    if (keys.has('KeyA')) camera.position.x -= speed;
    if (keys.has('KeyD')) camera.position.x += speed;
    if (keys.has('KeyQ')) camera.position.z -= speed;
    if (keys.has('KeyE')) camera.position.z += speed;
    return;
  } else {
    if (keys.has('KeyW')) camera.translateZ(-speed);
    if (keys.has('KeyS')) camera.translateZ(speed);
    if (keys.has('KeyA')) camera.translateX(-speed);
    if (keys.has('KeyD')) camera.translateX(speed);
  }
  if (keys.has('KeyQ')) camera.position.y -= speed;
  if (keys.has('KeyE')) camera.position.y += speed;
}

function makeCamera(mode, mount) {
  if (mode === 'orthographic') {
    const camera = new THREE.OrthographicCamera(-1, 1, 1, -1, -100000, 100000);
    camera.rotation.set(0, 0, 0);
    return camera;
  }

  return new THREE.PerspectiveCamera(55, mount.clientWidth / mount.clientHeight, 0.1, 100000);
}

function frameCamera(camera, bounds, mount, mode) {
  if (camera.isOrthographicCamera || mode === 'orthographic') {
    updateCameraProjection(camera, mount, bounds, 'orthographic');
    camera.position.set((bounds.minX + bounds.maxX) / 2, (bounds.minY + bounds.maxY) / 2, 10000);
    camera.rotation.set(0, 0, 0);
    camera.lookAt(camera.position.x, camera.position.y, 0);
    return;
  }

  const width = Math.max(1, bounds.maxX - bounds.minX);
  const height = Math.max(1, bounds.maxY - bounds.minY);
  const distance = Math.max(width, height, 1200) * 1.15;
  camera.position.set(0, 0, distance);
  camera.rotation.set(0, 0, 0);
}

function updateCameraProjection(camera, mount, bounds, mode) {
  const width = Math.max(1, mount.clientWidth);
  const height = Math.max(1, mount.clientHeight);
  const aspect = width / height;

  if (camera.isOrthographicCamera || mode === 'orthographic') {
    const sceneWidth = Math.max(1, bounds.maxX - bounds.minX) * ORTHO_PADDING;
    const sceneHeight = Math.max(1, bounds.maxY - bounds.minY) * ORTHO_PADDING;
    const viewHeight = Math.max(sceneHeight, sceneWidth / aspect, 600);
    const viewWidth = viewHeight * aspect;
    camera.left = -viewWidth / 2;
    camera.right = viewWidth / 2;
    camera.top = viewHeight / 2;
    camera.bottom = -viewHeight / 2;
    camera.near = -100000;
    camera.far = 100000;
    camera.updateProjectionMatrix();
    return;
  }

  camera.aspect = aspect;
  camera.updateProjectionMatrix();
}

function panOrthographicCamera(camera, dx, dy, mount) {
  const viewWidth = (camera.right - camera.left) / camera.zoom;
  const viewHeight = (camera.top - camera.bottom) / camera.zoom;
  camera.position.x -= (dx / Math.max(1, mount.clientWidth)) * viewWidth;
  camera.position.y += (dy / Math.max(1, mount.clientHeight)) * viewHeight;
}

function computeBounds(parsed) {
  const values = [];
  parsed?.sections.forEach((section) => {
    section.drawRecords.forEach((record) => {
      values.push(
        { x: record.x, y: record.y },
        { x: record.x + record.width, y: record.y + record.height },
      );
    });
    section.helperRecords.forEach((record) => {
      values.push(
        { x: record.x, y: record.y },
        { x: record.x + record.width, y: record.y + record.height },
      );
    });
  });
  parsed?.rich?.subresourceBank?.subresources.forEach((subresource) => {
    subresource.meshes.forEach((mesh) => {
      pushRectBounds(values, richMeshSpatialRect(subresource, mesh));
    });
    subresource.attachments.forEach((attachment) => pushRectBounds(values, attachmentSpatialRect(subresource, attachment)));
    subresource.controls.forEach((control) => pushRectBounds(values, controlSpatialRect(subresource, control)));
  });

  if (!values.length) {
    return { minX: -500, maxX: 500, minY: -350, maxY: 350 };
  }

  return {
    minX: Math.min(...values.map((v) => v.x)),
    maxX: Math.max(...values.map((v) => v.x)),
    minY: Math.min(...values.map((v) => v.y)),
    maxY: Math.max(...values.map((v) => v.y)),
  };
}

function pushRectBounds(values, rect) {
  values.push(
    { x: rect.x, y: rect.y },
    { x: rect.x + rect.width, y: rect.y + rect.height },
  );
}

function makeLabelTexture(text) {
  const canvas = document.createElement('canvas');
  canvas.width = 512;
  canvas.height = 512;
  const ctx = canvas.getContext('2d');
  ctx.fillStyle = '#1b1f27';
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  ctx.strokeStyle = '#e6c55a';
  ctx.lineWidth = 12;
  ctx.strokeRect(18, 18, canvas.width - 36, canvas.height - 36);
  ctx.fillStyle = '#f3f6fb';
  ctx.font = 'bold 44px Arial, sans-serif';
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  wrapCanvasText(ctx, text, canvas.width / 2, canvas.height / 2, 420, 54);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  texture.flipY = false;
  return texture;
}

function wrapCanvasText(ctx, text, x, y, maxWidth, lineHeight) {
  const words = String(text).split(/[_\s.-]+/).filter(Boolean);
  const lines = [];
  let current = '';
  words.forEach((word) => {
    const next = current ? `${current} ${word}` : word;
    if (ctx.measureText(next).width > maxWidth && current) {
      lines.push(current);
      current = word;
    } else {
      current = next;
    }
  });
  if (current) lines.push(current);
  const start = y - ((lines.length - 1) * lineHeight) / 2;
  lines.slice(0, 6).forEach((line, index) => ctx.fillText(line, x, start + index * lineHeight));
}

function polygonArea(vertices) {
  return (
    vertices[0].x * vertices[1].y - vertices[1].x * vertices[0].y +
    vertices[1].x * vertices[3].y - vertices[3].x * vertices[1].y +
    vertices[3].x * vertices[2].y - vertices[2].x * vertices[3].y +
    vertices[2].x * vertices[0].y - vertices[0].x * vertices[2].y
  ) / 2;
}

function averageAlpha(colors) {
  return colors.reduce((sum, color) => sum + color.a / 255, 0) / Math.max(colors.length, 1);
}

function makeTriangleStripIndices(vertexCount) {
  const indices = [];
  for (let i = 0; i < vertexCount - 2; i += 1) {
    if (i % 2 === 0) {
      indices.push(i, i + 1, i + 2);
    } else {
      indices.push(i + 1, i, i + 2);
    }
  }
  return indices;
}

function toColorFloat(color) {
  return [color.r / 255, color.g / 255, color.b / 255];
}

function depthOffset(record, index, base) {
  const depth = Number.isFinite(record.z) ? record.z * 0.02 : 0;
  return base + depth + index * 0.35;
}

function clearChildren(group) {
  while (group.children.length) {
    const child = group.children.pop();
    disposeObject(child);
  }
}

function disposeGroup(group) {
  clearChildren(group);
}

function disposeObject(object) {
  object.traverse?.((child) => {
    child.geometry?.dispose?.();
    if (Array.isArray(child.material)) {
      child.material.forEach((material) => material.dispose?.());
    } else {
      child.material?.dispose?.();
    }
  });
}

function clamp(value, min, max) {
  return Math.min(max, Math.max(min, value));
}

function formatHex(value) {
  return `0x${value.toString(16).toUpperCase()}`;
}

function makeTreeKey(type, sectionIndex, recordIndex) {
  return `${type}:${sectionIndex}:${recordIndex}`;
}

function makeGroupKey(sectionIndex, type) {
  return `group:${sectionIndex}:${type}`;
}

function isHidden(hiddenKeys, key) {
  return hiddenKeys?.includes(key);
}

function pickTransformObject(state, event) {
  const hit = pickSceneObject(state, event, state.handleObjects);
  if (hit?.object?.userData?.transform) return hit.object.userData.transform;

  const selectable = pickSceneObject(state, event, state.selectableObjects);
  const key = selectable?.object?.userData?.selectKey;
  return key ? { key, handle: 'move' } : null;
}

function pickObject(state, event, onSelect) {
  if (!onSelect || !state.selectableObjects.length) return;

  const hits = pickSceneObject(state, event, state.selectableObjects, true);
  const key = hits?.object?.userData?.selectKey ?? null;
  onSelect(key);
}

function pickSceneObject(state, event, objects) {
  if (!objects?.length) return null;
  const rect = state.renderer.domElement.getBoundingClientRect();
  state.pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
  state.pointer.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;
  state.raycaster.setFromCamera(state.pointer, state.camera);
  return state.raycaster.intersectObjects(objects, false)[0] ?? null;
}

function pointerToWorld(state, event) {
  const rect = state.renderer.domElement.getBoundingClientRect();
  state.pointer.x = ((event.clientX - rect.left) / rect.width) * 2 - 1;
  state.pointer.y = -((event.clientY - rect.top) / rect.height) * 2 + 1;
  state.raycaster.setFromCamera(state.pointer, state.camera);
  const plane = new THREE.Plane(new THREE.Vector3(0, 0, 1), 0);
  const out = new THREE.Vector3();
  state.raycaster.ray.intersectPlane(plane, out);
  return out;
}

function applyTransform(transform, dx, dy, onEditItem) {
  if (!onEditItem || (!dx && !dy)) return;
  onEditItem(transform.key, (draft) => transformSpatialDraft(draft, transform.handle, dx, dy));
}

function transformSpatialDraft(draft, handle, dxWorld, dyWorld) {
  if (draft.positions && draft.localOffset) return transformRichMeshDraft(draft, handle, dxWorld, dyWorld);
  if (draft.transform) {
    return {
      ...draft,
      transform: transformVec4RectDraft(draft.transform, handle, dxWorld, dyWorld),
    };
  }
  if (draft.rect) {
    return {
      ...draft,
      rect: transformRectDraft(draft.rect, handle, dxWorld, dyWorld),
    };
  }
  return transformRectDraft(draft, handle, dxWorld, dyWorld);
}

function transformVec4RectDraft(vec, handle, dxWorld, dyWorld) {
  const rect = {
    rawX: vec.rawX ?? 0,
    rawY: vec.rawY ?? 0,
    rawWidth: Math.max(1, Math.abs(vec.rawZ ?? 1)),
    rawHeight: Math.max(1, Math.abs(vec.rawW ?? 1)),
    x: (vec.rawX ?? 0) * RECT_SCALE,
    y: (vec.rawY ?? 0) * RECT_SCALE,
    width: Math.max(1, Math.abs(vec.rawZ ?? 1)) * RECT_SCALE,
    height: Math.max(1, Math.abs(vec.rawW ?? 1)) * RECT_SCALE,
  };
  const next = transformRectDraft(rect, handle, dxWorld, dyWorld);
  return {
    ...vec,
    rawX: next.rawX,
    rawY: next.rawY,
    rawZ: next.rawWidth,
    rawW: next.rawHeight,
    x: next.x,
    y: next.y,
    z: next.rawWidth,
    w: next.rawHeight,
  };
}

function transformRichMeshDraft(draft, handle, dxWorld, dyWorld) {
  const dxRaw = Math.round(dxWorld / RECT_SCALE);
  const dyRaw = Math.round(-dyWorld / RECT_SCALE);
  if (handle === 'move') {
    return {
      ...draft,
      localOffset: updateRawVec3(draft.localOffset, {
        rawX: draft.localOffset.rawX + dxRaw,
        rawY: draft.localOffset.rawY + dyRaw,
        rawZ: draft.localOffset.rawZ,
      }),
    };
  }

  const bounds = meshLocalBounds(draft);
  const rect = {
    rawX: draft.localOffset.rawX + bounds.minX,
    rawY: draft.localOffset.rawY + bounds.minY,
    rawWidth: Math.max(1, bounds.maxX - bounds.minX),
    rawHeight: Math.max(1, bounds.maxY - bounds.minY),
    x: (draft.localOffset.rawX + bounds.minX) * RECT_SCALE,
    y: (draft.localOffset.rawY + bounds.minY) * RECT_SCALE,
    width: Math.max(1, bounds.maxX - bounds.minX) * RECT_SCALE,
    height: Math.max(1, bounds.maxY - bounds.minY) * RECT_SCALE,
  };
  const next = transformRectDraft(rect, handle, dxWorld, dyWorld);
  const scaleX = next.rawWidth / Math.max(1, rect.rawWidth);
  const scaleY = next.rawHeight / Math.max(1, rect.rawHeight);
  return {
    ...draft,
    localOffset: updateRawVec3(draft.localOffset, {
      rawX: next.rawX,
      rawY: next.rawY,
      rawZ: draft.localOffset.rawZ,
    }),
    positions: draft.positions.map((position) => updateRawVec3(position, {
      rawX: Math.round((position.rawX - bounds.minX) * scaleX),
      rawY: Math.round((position.rawY - bounds.minY) * scaleY),
      rawZ: position.rawZ,
    })),
  };
}

function meshLocalBounds(mesh) {
  const xs = mesh.positions.map((position) => position.rawX);
  const ys = mesh.positions.map((position) => position.rawY);
  return {
    minX: Math.min(...xs, 0),
    minY: Math.min(...ys, 0),
    maxX: Math.max(...xs, 1),
    maxY: Math.max(...ys, 1),
  };
}

function updateRawVec3(vec, raw) {
  return {
    ...vec,
    ...raw,
    x: raw.rawX * RECT_SCALE,
    y: raw.rawY * RECT_SCALE,
    z: raw.rawZ,
  };
}

function transformRectDraft(draft, handle, dxWorld, dyWorld) {
  const dxRaw = Math.round(dxWorld / RECT_SCALE);
  const dyRaw = Math.round(-dyWorld / RECT_SCALE);
  if (handle === 'move') {
    return updateRawRect(draft, {
      rawX: draft.rawX + dxRaw,
      rawY: draft.rawY + dyRaw,
      rawWidth: draft.rawWidth,
      rawHeight: draft.rawHeight,
    });
  }

  let rawX = draft.rawX;
  let rawY = draft.rawY;
  let rawWidth = draft.rawWidth;
  let rawHeight = draft.rawHeight;

  if (handle.includes('e')) rawWidth += dxRaw;
  if (handle.includes('s')) rawHeight += dyRaw;
  if (handle.includes('w')) {
    rawX += dxRaw;
    rawWidth -= dxRaw;
  }
  if (handle.includes('n')) {
    rawY += dyRaw;
    rawHeight -= dyRaw;
  }

  if (isCornerHandle(handle)) {
    const aspect = Math.max(1, Math.abs(draft.rawWidth)) / Math.max(1, Math.abs(draft.rawHeight));
    if (Math.abs(rawWidth - draft.rawWidth) > Math.abs(rawHeight - draft.rawHeight)) {
      rawHeight = Math.round(Math.sign(rawHeight || 1) * Math.abs(rawWidth) / aspect);
      if (handle.includes('n')) rawY = draft.rawY + draft.rawHeight - rawHeight;
    } else {
      rawWidth = Math.round(Math.sign(rawWidth || 1) * Math.abs(rawHeight) * aspect);
      if (handle.includes('w')) rawX = draft.rawX + draft.rawWidth - rawWidth;
    }
  }

  return updateRawRect(draft, normalizeRawRect(rawX, rawY, rawWidth, rawHeight));
}

function normalizeRawRect(rawX, rawY, rawWidth, rawHeight) {
  if (rawWidth < 1) {
    rawX += rawWidth - 1;
    rawWidth = 1;
  }
  if (rawHeight < 1) {
    rawY += rawHeight - 1;
    rawHeight = 1;
  }
  return { rawX, rawY, rawWidth, rawHeight };
}

function updateRawRect(draft, rect) {
  return {
    ...draft,
    ...rect,
    x: rect.rawX * RECT_SCALE,
    y: rect.rawY * RECT_SCALE,
    width: rect.rawWidth * RECT_SCALE,
    height: rect.rawHeight * RECT_SCALE,
  };
}

function isCornerHandle(handle) {
  return ['nw', 'ne', 'se', 'sw'].includes(handle);
}
