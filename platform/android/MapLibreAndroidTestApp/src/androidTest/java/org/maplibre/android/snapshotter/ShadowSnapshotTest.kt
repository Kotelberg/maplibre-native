package org.maplibre.android.snapshotter

import android.graphics.Bitmap
import androidx.test.internal.runner.junit4.AndroidJUnit4ClassRunner
import androidx.test.platform.app.InstrumentationRegistry
import androidx.test.rule.ActivityTestRule
import org.junit.Assert
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.Style
import org.maplibre.android.testapp.activity.FeatureOverviewActivity
import java.io.File
import java.io.FileOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Headless A/B harness for the 3D-building shadow port (GL backend).
 *
 * Renders the live HataHub Kyiv style at a fixed pitched camera via MapSnapshotter and writes a PNG
 * to the app's external files dir, which is then pulled with `adb pull`. This is the Android analog
 * of the Metal `mbgl-render -o x.png` loop used to develop the iOS shadows.
 *
 *   ./gradlew :MapLibreAndroidTestApp:installOpenglDebug \
 *             :MapLibreAndroidTestApp:installOpenglDebugAndroidTest
 *   adb shell am instrument -w \
 *     -e class org.maplibre.android.snapshotter.ShadowSnapshotTest#kyivShadows \
 *     org.maplibre.android.testapp.test/org.maplibre.android.InstrumentationRunner
 *   adb pull /sdcard/Android/data/org.maplibre.android.testapp/files/shadow_kyiv.png /tmp/
 */
@RunWith(AndroidJUnit4ClassRunner::class)
class ShadowSnapshotTest {

    @Rule
    @JvmField
    var rule = ActivityTestRule(FeatureOverviewActivity::class.java)

    private val latch = CountDownLatch(1)

    @Test
    fun kyivShadows() {
        // Prefer a local debug style pushed to the app's external files dir (lets me tune
        // sun angle / shadow-intensity without rebuilding); fall back to the live prod style.
        val localStyle = File(rule.activity.getExternalFilesDir(null), "debug_style.json")
        val styleBuilder = if (localStyle.exists()) {
            Style.Builder().fromUri("file://" + localStyle.absolutePath)
        } else {
            Style.Builder().fromUri("https://map.hatahub.com.ua/style/light.json")
        }
        // Camera overridable via `-e zoom/pitch/bearing/lat/lon <v>` so angles can be swept without
        // rebuilding. Defaults: Maidan Nezalezhnosti — dense, varied heights for clear cast shadows.
        val args = InstrumentationRegistry.getArguments()
        val camera = CameraPosition.Builder()
            .target(LatLng(args.getString("lat")?.toDouble() ?: 50.4500,
                           args.getString("lon")?.toDouble() ?: 30.5236))
            .zoom(args.getString("zoom")?.toDouble() ?: 18.0)
            .tilt(args.getString("pitch")?.toDouble() ?: 55.0)
            .bearing(args.getString("bearing")?.toDouble() ?: 25.0)
            .build()

        var snapshotter: MapSnapshotter? = null
        var error: String? = null

        rule.activity.runOnUiThread {
            val options = MapSnapshotter.Options(1024, 768)
                .withPixelRatio(1.0f)
                .withStyleBuilder(styleBuilder)
                .withCameraPosition(camera)
            snapshotter = MapSnapshotter(rule.activity, options)
            snapshotter!!.start(
                { snapshot ->
                    val dir = rule.activity.getExternalFilesDir(null)
                    val file = File(dir, "shadow_kyiv.png")
                    FileOutputStream(file).use { out ->
                        snapshot.bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
                    }
                    latch.countDown()
                },
                { err ->
                    error = err
                    latch.countDown()
                }
            )
        }

        val finished = latch.await(120, TimeUnit.SECONDS)
        Assert.assertTrue("snapshot timed out (network tiles?)", finished)
        Assert.assertNull("snapshot error: $error", error)
    }
}
