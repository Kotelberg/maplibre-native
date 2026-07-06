package org.maplibre.android.maps.renderer.egl;

import android.opengl.GLSurfaceView;
import android.os.Build;

import androidx.annotation.NonNull;

import org.maplibre.android.constants.MapLibreConstants;
import org.maplibre.android.log.Logger;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import javax.microedition.khronos.egl.EGL10;
import javax.microedition.khronos.egl.EGLConfig;
import javax.microedition.khronos.egl.EGLDisplay;

import static org.maplibre.android.utils.Compare.compare;
import static javax.microedition.khronos.egl.EGL10.EGL_ALPHA_MASK_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_ALPHA_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_BLUE_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_BUFFER_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_COLOR_BUFFER_TYPE;
import static javax.microedition.khronos.egl.EGL10.EGL_CONFIG_CAVEAT;
import static javax.microedition.khronos.egl.EGL10.EGL_DEPTH_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_GREEN_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_NONE;
import static javax.microedition.khronos.egl.EGL10.EGL_RED_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_RENDERABLE_TYPE;
import static javax.microedition.khronos.egl.EGL10.EGL_RGB_BUFFER;
import static javax.microedition.khronos.egl.EGL10.EGL_SAMPLES;
import static javax.microedition.khronos.egl.EGL10.EGL_SAMPLE_BUFFERS;
import static javax.microedition.khronos.egl.EGL10.EGL_STENCIL_SIZE;
import static javax.microedition.khronos.egl.EGL10.EGL_SURFACE_TYPE;
import static javax.microedition.khronos.egl.EGL10.EGL_WINDOW_BIT;

/**
 * Selects the right EGLConfig needed for `mapbox-gl-native`
 */
public class EGLConfigChooser implements GLSurfaceView.EGLConfigChooser {

  private static final String TAG = "Mbgl-EGLConfigChooser";

  /**
   * Requires API level 18
   *
   * @see android.opengl.EGL15.EGL_OPENGL_ES3_BIT;
   */
  @SuppressWarnings("JavadocReference")
  private static final int EGL_OPENGL_ES3_BIT = 0x0040;

  /**
   * Sample count meaning "no multisampling" — the legacy, pre-MSAA behavior. This is the value
   * used by the default constructor and the single-{@code boolean} constructor, so callers that
   * don't opt into MSAA keep the original (samples == 0) config-selection behavior unchanged.
   */
  public static final int MSAA_SAMPLES_DISABLED = 1;

  private final boolean translucentSurface;
  private final int desiredSamples;

  public EGLConfigChooser() {
    this(false);
  }

  public EGLConfigChooser(boolean translucentSurface) {
    this(translucentSurface, MSAA_SAMPLES_DISABLED);
  }

  /**
   * @param translucentSurface whether the window surface needs an alpha channel.
   * @param desiredSamples     preferred MSAA sample count for the on-screen framebuffer. 3D
   *                           fill-extrusion (building) edges are hard polygon edges with no
   *                           shader-side antialiasing — unlike SDF lines/fills — so without MSAA
   *                           they can alias ("pixelated edges"). When {@code desiredSamples > 1},
   *                           the chooser prefers an MSAA config near this count (clamped to a sane
   *                           mobile range of 2..8 samples) and falls back to a non-MSAA config when
   *                           the driver offers none. Values {@code <= 1} disable MSAA and restore
   *                           the legacy (samples == 0) selection behavior exactly.
   */
  public EGLConfigChooser(boolean translucentSurface, int desiredSamples) {
    super();
    this.translucentSurface = translucentSurface;
    this.desiredSamples = desiredSamples;
  }

  @Override
  public EGLConfig chooseConfig(@NonNull EGL10 egl, EGLDisplay display) {
    int[] configAttribs = getConfigAttributes();

    // Determine number of possible configurations
    int[] numConfigs = getNumberOfConfigurations(egl, display, configAttribs);
    if (numConfigs[0] < 1) {
      Logger.e(TAG, "eglChooseConfig() returned no configs.");
    }

    // Get all possible configurations
    EGLConfig[] possibleConfigurations = getPossibleConfigurations(egl, display, configAttribs, numConfigs);

    // Choose best match
    EGLConfig config = chooseBestMatchConfig(egl, display, possibleConfigurations);
    if (config == null) {
      Logger.e(TAG, "No config chosen");
    }

    return config;
  }

  @NonNull
  private int[] getNumberOfConfigurations(EGL10 egl, EGLDisplay display, int[] configAttributes) {
    int[] numConfigs = new int[1];
    if (!egl.eglChooseConfig(display, configAttributes, null, 0, numConfigs)) {
      Logger.e(TAG, String.format(
        MapLibreConstants.MAPLIBRE_LOCALE, "eglChooseConfig(NULL) returned error %d", egl.eglGetError())
      );
    }
    return numConfigs;
  }

  @NonNull
  private EGLConfig[] getPossibleConfigurations(EGL10 egl, EGLDisplay display,
                                                int[] configAttributes, int[] numConfigs) {
    EGLConfig[] configs = new EGLConfig[numConfigs[0]];
    if (!egl.eglChooseConfig(display, configAttributes, configs, numConfigs[0], numConfigs)) {
      Logger.e(TAG, String.format(
        MapLibreConstants.MAPLIBRE_LOCALE, "eglChooseConfig() returned error %d", egl.eglGetError())
      );
    }
    return configs;
  }

  // Quality
  enum BufferFormat {
    Format16Bit(3),
    Format32BitNoAlpha(1),
    Format32BitAlpha(2),
    Format24Bit(0),
    Unknown(4);

    int value;

    BufferFormat(int value) {
      this.value = value;
    }
  }

  enum DepthStencilFormat {
    Format16Depth8Stencil(1),
    Format24Depth8Stencil(0);

    int value;

    DepthStencilFormat(int value) {
      this.value = value;
    }
  }

  private EGLConfig chooseBestMatchConfig(@NonNull EGL10 egl, EGLDisplay display, EGLConfig[] configs) {
    class Config implements Comparable<Config> {
      private final BufferFormat bufferFormat;
      private final DepthStencilFormat depthStencilFormat;
      private final boolean isCaveat;
      private final int samples;
      private final int index;
      private final EGLConfig config;

      public Config(BufferFormat bufferFormat, DepthStencilFormat depthStencilFormat,
                    boolean isCaveat, int samples, int index, EGLConfig config) {
        this.bufferFormat = bufferFormat;
        this.depthStencilFormat = depthStencilFormat;
        this.isCaveat = isCaveat;
        this.samples = samples;
        this.index = index;
        this.config = config;
      }

      // Lower is better. Prefer the requested MSAA sample count, then the closest other MSAA
      // count, and a non-MSAA (samples <= 1) config only as a last resort — so an anti-aliased
      // framebuffer wins whenever MSAA was requested and the driver offers one. When MSAA wasn't
      // requested (desiredSamples <= 1), every candidate reaching this point already has
      // samples == 0 (see the noMsaa/wantMsaa filter below), so this rank ties for all of them and
      // has no effect on ordering — preserving the legacy comparator behavior exactly.
      private int msaaRank() {
        if (samples <= 1) {
          return Integer.MAX_VALUE;
        }
        if (samples == desiredSamples) {
          return 0;
        }
        return 1 + Math.abs(samples - desiredSamples);
      }

      @Override
      public int compareTo(@NonNull Config other) {
        int i = compare(msaaRank(), other.msaaRank());
        if (i != 0) {
          return i;
        }

        i = compare(bufferFormat.value, other.bufferFormat.value);
        if (i != 0) {
          return i;
        }

        i = compare(depthStencilFormat.value, other.depthStencilFormat.value);
        if (i != 0) {
          return i;
        }

        i = compare(isCaveat, other.isCaveat);
        if (i != 0) {
          return i;
        }

        i = compare(index, other.index);
        if (i != 0) {
          return i;
        }

        return 0;
      }
    }

    List<Config> matches = new ArrayList<>();

    int i = 0;
    for (EGLConfig config : configs) {
      if (config == null) {
        continue;
      }

      i++;

      int caveat = getConfigAttr(egl, display, config, EGL_CONFIG_CAVEAT);
      int bits = getConfigAttr(egl, display, config, EGL_BUFFER_SIZE);
      int red = getConfigAttr(egl, display, config, EGL_RED_SIZE);
      int green = getConfigAttr(egl, display, config, EGL_GREEN_SIZE);
      int blue = getConfigAttr(egl, display, config, EGL_BLUE_SIZE);
      int alpha = getConfigAttr(egl, display, config, EGL_ALPHA_SIZE);
      int alphaMask = getConfigAttr(egl, display, config, EGL_ALPHA_MASK_SIZE);
      int depth = getConfigAttr(egl, display, config, EGL_DEPTH_SIZE);
      int stencil = getConfigAttr(egl, display, config, EGL_STENCIL_SIZE);
      int sampleBuffers = getConfigAttr(egl, display, config, EGL_SAMPLE_BUFFERS);
      int samples = getConfigAttr(egl, display, config, EGL_SAMPLES);

      boolean configOk = (depth == 24) || (depth == 16);
      configOk &= stencil == 8;
      // Accept a non-MSAA config (the legacy requirement, byte-identical when desiredSamples <= 1)
      // OR — only when MSAA was explicitly requested — an MSAA config in a sane mobile range
      // (2..8 samples; avoids e.g. a driver's 16x config, far too expensive on a tiled mobile GPU).
      // The comparator (msaaRank) then prefers MSAA over non-MSAA and the requested sample count
      // over the rest, with non-MSAA kept only as a fallback.
      boolean noMsaa = (sampleBuffers == 0) && (samples == 0);
      boolean wantMsaa = (desiredSamples > 1) && (sampleBuffers >= 1) && (samples >= 2) && (samples <= 8);
      configOk &= (noMsaa || wantMsaa);

      // Filter our configs first for depth, stencil and anti-aliasing
      if (configOk) {
        // Work out the config's buffer format
        BufferFormat bufferFormat;
        if ((bits == 16) && (red == 5) && (green == 6) && (blue == 5) && (alpha == 0)) {
          bufferFormat = BufferFormat.Format16Bit;
        } else if ((bits == 32) && (red == 8) && (green == 8) && (blue == 8) && (alpha == 0)) {
          bufferFormat = BufferFormat.Format32BitNoAlpha;
        } else if ((bits == 32) && (red == 8) && (green == 8) && (blue == 8) && (alpha == 8)) {
          bufferFormat = BufferFormat.Format32BitAlpha;
        } else if ((bits == 24) && (red == 8) && (green == 8) && (blue == 8) && (alpha == 0)) {
          bufferFormat = BufferFormat.Format24Bit;
        } else {
          bufferFormat = BufferFormat.Unknown;
        }

        // Work out the config's depth stencil format
        DepthStencilFormat depthStencilFormat;
        if ((depth == 16) && (stencil == 8)) {
          depthStencilFormat = DepthStencilFormat.Format16Depth8Stencil;
        } else {
          depthStencilFormat = DepthStencilFormat.Format24Depth8Stencil;
        }

        boolean isCaveat = caveat != EGL_NONE;

        // Ignore formats we don't recognise
        if (bufferFormat != BufferFormat.Unknown) {
          matches.add(new Config(bufferFormat, depthStencilFormat, isCaveat, samples, i, config));
        }
      }

    }

    // Sort
    Collections.sort(matches);

    if (matches.size() == 0) {
      Logger.e(TAG, "No matching configurations after filtering");
      return null;
    }

    Config bestMatch = matches.get(0);

    if (desiredSamples > 1) {
      Logger.i(TAG, String.format(MapLibreConstants.MAPLIBRE_LOCALE,
        "Chosen EGL config: bufferFormat=%s depthStencil=%s samples=%d (MSAA requested=%d)",
        bestMatch.bufferFormat, bestMatch.depthStencilFormat, bestMatch.samples, desiredSamples));
    }

    if (bestMatch.isCaveat) {
      Logger.w(TAG, "Chosen config has a caveat.");
    }

    return bestMatch.config;
  }

  private int getConfigAttr(EGL10 egl, EGLDisplay display, EGLConfig config, int attributeName) {
    int[] attributevalue = new int[1];
    if (!egl.eglGetConfigAttrib(display, config, attributeName, attributevalue)) {
      Logger.e(TAG, String.format(
        MapLibreConstants.MAPLIBRE_LOCALE, "eglGetConfigAttrib(%d) returned error %d", attributeName, egl.eglGetError())
      );
    }
    return attributevalue[0];
  }

  private int[] getConfigAttributes() {
    boolean emulator = inEmulator() || inGenymotion();
    Logger.i(TAG, String.format("In emulator: %s", emulator));

    // Get all configs at least RGB 565 with 16 depth and 8 stencil
    return new int[] {
      EGL_CONFIG_CAVEAT, EGL_NONE,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
      EGL_BUFFER_SIZE, 16,
      EGL_RED_SIZE, 5,
      EGL_GREEN_SIZE, 6,
      EGL_BLUE_SIZE, 5,
      EGL_ALPHA_SIZE, translucentSurface ? 8 : 0,
      EGL_DEPTH_SIZE, 16,
      EGL_STENCIL_SIZE, 8,
      (emulator ? EGL_NONE : EGL_COLOR_BUFFER_TYPE), EGL_RGB_BUFFER,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
      EGL_NONE
    };
  }

  /**
   * Detect if we are in emulator.
   */
  private boolean inEmulator() {
    return Build.FINGERPRINT.startsWith("generic")
      || Build.FINGERPRINT.startsWith("unknown")
      || Build.MODEL.contains("google_sdk")
      || Build.MODEL.contains("Emulator")
      || Build.MODEL.contains("Android SDK built for x86")
      || (Build.BRAND.startsWith("generic") && Build.DEVICE.startsWith("generic"))
      || "google_sdk".equals(Build.PRODUCT)
      || System.getProperty("ro.kernel.qemu") != null;
  }

  /**
   * Detect if we are in genymotion
   */
  private boolean inGenymotion() {
    return Build.MANUFACTURER.contains("Genymotion");
  }

}
