// This file is generated. Edit scripts/generate-style-code.js, then run `make style-code`.

package org.maplibre.android.style.layers;

import androidx.annotation.ColorInt;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.annotation.UiThread;

import static org.maplibre.android.utils.ColorUtils.rgbaToColor;

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import org.maplibre.android.style.expressions.Expression;
import org.maplibre.android.style.layers.TransitionOptions;
import java.util.HashMap;
import java.util.Map;

/**
 * Experimental. Places a 3D glTF/GLB model at each Point of a GeoJSON source. The model drawn for a feature is selected by the `model-id` property and resolved through a host-supplied runtime asset registry (the SDK `model-assets` API); the style itself references models only by id. Rendered with the map's built-in custom-drawable pipeline, so no additional shaders are required.
 *
 * @see <a href="https://maplibre.org/maplibre-style-spec/#layers-model">The online documentation</a>
 */
@UiThread
public class ModelLayer extends Layer {

  /**
   * Creates a ModelLayer.
   *
   * @param nativePtr pointer used by core
   */
  @Keep
  ModelLayer(long nativePtr) {
    super(nativePtr);
  }

  /**
   * Creates a ModelLayer.
   *
   * @param layerId  the id of the layer
   * @param sourceId the id of the source
   */
  public ModelLayer(String layerId, String sourceId) {
    super();
    initialize(layerId, sourceId);
  }

  @Keep
  protected native void initialize(String layerId, String sourceId);

  /**
   * Set the source layer.
   *
   * @param sourceLayer the source layer to set
   */
  public void setSourceLayer(String sourceLayer) {
    checkThread();
    nativeSetSourceLayer(sourceLayer);
  }

  /**
   * Set the source Layer.
   *
   * @param sourceLayer the source layer to set
   * @return This
   */
  @NonNull
  public ModelLayer withSourceLayer(String sourceLayer) {
    setSourceLayer(sourceLayer);
    return this;
  }

  /**
   * Get the source id.
   *
   * @return id of the source
   */
  @NonNull
  public String getSourceId() {
    checkThread();
    return nativeGetSourceId();
  }

  /**
   * Get the source layer.
   *
   * @return sourceLayer the source layer to get
   */
  @NonNull
  public String getSourceLayer() {
    checkThread();
    return nativeGetSourceLayer();
  }

  /**
   * Set a single expression filter.
   *
   * @param filter the expression filter to set
   */
  public void setFilter(@NonNull Expression filter) {
    checkThread();
    nativeSetFilter(filter.toArray());
  }

  /**
   * Set a single expression filter.
   *
   * @param filter the expression filter to set
   * @return This
   */
  @NonNull
  public ModelLayer withFilter(@NonNull Expression filter) {
    setFilter(filter);
    return this;
  }

  /**
   * Get a single expression filter.
   *
   * @return the expression filter to get
   */
  @Nullable
  public Expression getFilter() {
    checkThread();
    JsonElement jsonElement = nativeGetFilter();
    if (jsonElement != null) {
      return Expression.Converter.convert(jsonElement);
    } else {
      return null;
    }
  }

  /**
   * Set a property or properties.
   *
   * @param properties the var-args properties
   * @return This
   */
  @NonNull
  public ModelLayer withProperties(@NonNull PropertyValue<?>... properties) {
    setProperties(properties);
    return this;
  }

  // Property getters

  /**
   * Get the ModelId property
   *
   * @return property wrapper value around String
   */
  @NonNull
  @SuppressWarnings("unchecked")
  public PropertyValue<String> getModelId() {
    checkThread();
    return (PropertyValue<String>) new PropertyValue("model-id", nativeGetModelId());
  }

  /**
   * Get the ModelOpacity property
   *
   * @return property wrapper value around Float
   */
  @NonNull
  @SuppressWarnings("unchecked")
  public PropertyValue<Float> getModelOpacity() {
    checkThread();
    return (PropertyValue<Float>) new PropertyValue("model-opacity", nativeGetModelOpacity());
  }

  /**
   * Get the ModelOpacity property transition options
   *
   * @return transition options for Float
   */
  @NonNull
  public TransitionOptions getModelOpacityTransition() {
    checkThread();
    return nativeGetModelOpacityTransition();
  }

  /**
   * Set the ModelOpacity property transition options
   *
   * @param options transition options for Float
   */
  public void setModelOpacityTransition(@NonNull TransitionOptions options) {
    checkThread();
    nativeSetModelOpacityTransition(options.getDuration(), options.getDelay());
  }

  /**
   * Get the ModelScale property
   *
   * @return property wrapper value around Float
   */
  @NonNull
  @SuppressWarnings("unchecked")
  public PropertyValue<Float> getModelScale() {
    checkThread();
    return (PropertyValue<Float>) new PropertyValue("model-scale", nativeGetModelScale());
  }

  /**
   * Get the ModelScale property transition options
   *
   * @return transition options for Float
   */
  @NonNull
  public TransitionOptions getModelScaleTransition() {
    checkThread();
    return nativeGetModelScaleTransition();
  }

  /**
   * Set the ModelScale property transition options
   *
   * @param options transition options for Float
   */
  public void setModelScaleTransition(@NonNull TransitionOptions options) {
    checkThread();
    nativeSetModelScaleTransition(options.getDuration(), options.getDelay());
  }

  /**
   * Get the ModelRotation property
   *
   * @return property wrapper value around Float
   */
  @NonNull
  @SuppressWarnings("unchecked")
  public PropertyValue<Float> getModelRotation() {
    checkThread();
    return (PropertyValue<Float>) new PropertyValue("model-rotation", nativeGetModelRotation());
  }

  /**
   * Get the ModelRotation property transition options
   *
   * @return transition options for Float
   */
  @NonNull
  public TransitionOptions getModelRotationTransition() {
    checkThread();
    return nativeGetModelRotationTransition();
  }

  /**
   * Set the ModelRotation property transition options
   *
   * @param options transition options for Float
   */
  public void setModelRotationTransition(@NonNull TransitionOptions options) {
    checkThread();
    nativeSetModelRotationTransition(options.getDuration(), options.getDelay());
  }

  /**
   * Get the ModelFootprint property
   *
   * @return property wrapper value around Float
   */
  @NonNull
  @SuppressWarnings("unchecked")
  public PropertyValue<Float> getModelFootprint() {
    checkThread();
    return (PropertyValue<Float>) new PropertyValue("model-footprint", nativeGetModelFootprint());
  }

  /**
   * Get the ModelFootprint property transition options
   *
   * @return transition options for Float
   */
  @NonNull
  public TransitionOptions getModelFootprintTransition() {
    checkThread();
    return nativeGetModelFootprintTransition();
  }

  /**
   * Set the ModelFootprint property transition options
   *
   * @param options transition options for Float
   */
  public void setModelFootprintTransition(@NonNull TransitionOptions options) {
    checkThread();
    nativeSetModelFootprintTransition(options.getDuration(), options.getDelay());
  }

  /**
   * Maps model asset identifiers to local glTF/GLB file paths.
   *
   * <p>This is a runtime-only registry, mirroring {@code Style#addImage}: the
   * style references a model only by its {@code model-id}, while the host
   * application registers the file each id resolves to. Assets registered
   * here are never written to or read from the style JSON.
   *
   * @return a copy of the current asset registry
   */
  @NonNull
  public Map<String, String> getModelAssets() {
    checkThread();
    String[] flat = nativeGetModelAssets();
    Map<String, String> assets = new HashMap<>();
    for (int i = 0; i + 1 < flat.length; i += 2) {
      assets.put(flat[i], flat[i + 1]);
    }
    return assets;
  }

  /**
   * Replace the model asset registry. See {@link #getModelAssets()}.
   *
   * @param assets map of asset identifier to local glTF/GLB file path
   */
  public void setModelAssets(@NonNull Map<String, String> assets) {
    checkThread();
    String[] ids = assets.keySet().toArray(new String[0]);
    String[] paths = new String[ids.length];
    for (int i = 0; i < ids.length; i++) {
      paths[i] = assets.get(ids[i]);
    }
    nativeSetModelAssets(ids, paths);
  }

  @NonNull
  @Keep
  private native Object nativeGetModelId();

  @NonNull
  @Keep
  private native Object nativeGetModelOpacity();

  @NonNull
  @Keep
  private native TransitionOptions nativeGetModelOpacityTransition();

  @Keep
  private native void nativeSetModelOpacityTransition(long duration, long delay);

  @NonNull
  @Keep
  private native Object nativeGetModelScale();

  @NonNull
  @Keep
  private native TransitionOptions nativeGetModelScaleTransition();

  @Keep
  private native void nativeSetModelScaleTransition(long duration, long delay);

  @NonNull
  @Keep
  private native Object nativeGetModelRotation();

  @NonNull
  @Keep
  private native TransitionOptions nativeGetModelRotationTransition();

  @Keep
  private native void nativeSetModelRotationTransition(long duration, long delay);

  @NonNull
  @Keep
  private native Object nativeGetModelFootprint();

  @NonNull
  @Keep
  private native TransitionOptions nativeGetModelFootprintTransition();

  @Keep
  private native void nativeSetModelFootprintTransition(long duration, long delay);

  @NonNull
  @Keep
  private native String[] nativeGetModelAssets();

  @Keep
  private native void nativeSetModelAssets(String[] assetIds, String[] assetPaths);

  @Override
  @Keep
  protected native void finalize() throws Throwable;

}
