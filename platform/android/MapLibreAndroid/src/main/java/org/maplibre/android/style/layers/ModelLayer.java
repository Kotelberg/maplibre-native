package org.maplibre.android.style.layers;

import androidx.annotation.Keep;

/**
 * Experimental `model` style layer (fork extension, M6): renders a glTF model
 * per Point feature of a GeoJSON source via Filament. The convenience
 * constructor registers the GLB at {@code glbPath} and reads per-feature
 * {@code bearing}/{@code size} properties for rotation and scale.
 */
public class ModelLayer extends Layer {

  public ModelLayer(String id, String sourceId, String glbPath) {
    initialize(id, sourceId, glbPath);
  }

  @Keep
  ModelLayer(long nativePtr) {
    super(nativePtr);
  }

  @Keep
  protected native void initialize(String id, String sourceId, String glbPath);

  @Override
  @Keep
  protected native void finalize() throws Throwable;

}
