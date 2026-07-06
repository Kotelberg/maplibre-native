package org.maplibre.android.maps.renderer.surfaceview;

import android.content.Context;
import androidx.annotation.NonNull;

public class VulkanSurfaceViewMapRenderer extends SurfaceViewMapRenderer {

  public VulkanSurfaceViewMapRenderer(Context context,
                                @NonNull MapLibreVulkanSurfaceView surfaceView,
                                String localIdeographFontFamily) {
    this(context, surfaceView, localIdeographFontFamily, 1);
  }

  public VulkanSurfaceViewMapRenderer(Context context,
                                @NonNull MapLibreVulkanSurfaceView surfaceView,
                                String localIdeographFontFamily,
                                int msaaSamples) {
    super(context, surfaceView, localIdeographFontFamily, msaaSamples);

    this.surfaceView.setRenderer(this);
  }

}
