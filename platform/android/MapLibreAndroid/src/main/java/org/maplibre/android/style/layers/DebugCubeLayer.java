package org.maplibre.android.style.layers;

import androidx.annotation.Keep;

/**
 * M1 debug cube layer: renders a fixed-size cube at a coordinate to validate
 * placement and depth ahead of the model layer. Development only.
 */
public class DebugCubeLayer extends Layer {

  public DebugCubeLayer(String id, double lat, double lon, double sizeMeters) {
    initialize(id, lat, lon, sizeMeters);
  }

  @Keep
  DebugCubeLayer(long nativePtr) {
    super(nativePtr);
  }

  @Keep
  protected native void initialize(String id, double lat, double lon, double sizeMeters);

  @Override
  @Keep
  protected native void finalize() throws Throwable;

}
