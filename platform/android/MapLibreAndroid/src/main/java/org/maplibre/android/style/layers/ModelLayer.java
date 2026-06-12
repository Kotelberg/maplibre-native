package org.maplibre.android.style.layers;

import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;

import java.util.Map;

/**
 * Experimental `model` style layer (fork extension, M6): renders a glTF model
 * per Point feature of a GeoJSON source. Per-feature placement is data-driven
 * through the {@code bearing}/{@code size}/{@code footprint} feature
 * properties. When {@code modelID} is null, each feature's {@code model-id}
 * property selects the asset; a non-null value pins one asset for all
 * features.
 */
public class ModelLayer extends Layer {

  public ModelLayer(String id, String sourceId, String glbPath) {
    this(id, sourceId, new String[] {"demo"}, new String[] {glbPath}, "demo");
  }

  public ModelLayer(
      @NonNull String id,
      @NonNull String sourceId,
      @NonNull Map<String, String> assets,
      @Nullable String modelID) {
    initialize(id, sourceId, keysOf(assets), valuesOf(assets), modelID);
  }

  public ModelLayer(
      String id, String sourceId, String[] assetIds, String[] assetPaths, String modelID) {
    initialize(id, sourceId, assetIds, assetPaths, modelID);
  }

  // Iteration order is stable for a given map instance, so keysOf/valuesOf
  // produce aligned arrays.
  private static String[] keysOf(Map<String, String> assets) {
    return assets.keySet().toArray(new String[0]);
  }

  private static String[] valuesOf(Map<String, String> assets) {
    String[] keys = keysOf(assets);
    String[] values = new String[keys.length];
    for (int i = 0; i < keys.length; i++) {
      values[i] = assets.get(keys[i]);
    }
    return values;
  }

  @Keep
  ModelLayer(long nativePtr) {
    super(nativePtr);
  }

  @Keep
  protected native void initialize(
      String id, String sourceId, String[] assetIds, String[] assetPaths, String modelID);

  @Override
  @Keep
  protected native void finalize() throws Throwable;

}
