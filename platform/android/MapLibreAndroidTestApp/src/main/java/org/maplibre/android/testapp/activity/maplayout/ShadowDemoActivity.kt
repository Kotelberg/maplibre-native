package org.maplibre.android.testapp.activity.maplayout

import android.graphics.Point
import android.os.Bundle
import android.view.Gravity
import android.widget.Button
import android.widget.FrameLayout
import android.widget.LinearLayout
import androidx.appcompat.app.AppCompatActivity
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.Style
import org.maplibre.android.testapp.R

/**
 * Interactive on-device demo for the OpenGL 3D-building cast-shadow port.
 *
 * Opens straight to a pitched Kyiv map using a bundled style (`assets/shadow_demo_style.json`) with
 * `cast-shadows` enabled and a low SE sun (shadow-intensity amped to 0.6 for clear visibility — prod
 * uses ~0.12). Pan / pinch / rotate to verify the cast shadows are world-anchored (the sun stays put
 * as you rotate) and attached to building bases. Set as the launcher activity for this verify build.
 */
class ShadowDemoActivity : AppCompatActivity() {
    private lateinit var mapView: MapView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_map_simple)
        mapView = findViewById(R.id.mapView)
        mapView.onCreate(savedInstanceState)
        mapView.getMapAsync { map ->
            map.setStyle(Style.Builder().fromUri("asset://shadow_demo_style.json"))
            // Use moveCamera(CameraUpdateFactory...) — the same mechanism as the working zoom buttons.
            // The `map.cameraPosition =` setter did not apply at init on this build. z17.5 + 60° pitch
            // puts us above the z16 extrusion ramp and angled enough to see building sides + shadows.
            map.moveCamera(
                CameraUpdateFactory.newCameraPosition(
                    CameraPosition.Builder()
                        .target(LatLng(50.4500, 30.5236)) // Maidan Nezalezhnosti
                        .zoom(17.5)
                        .tilt(60.0)
                        .bearing(25.0)
                        .build()
                )
            )

            // Zoom +/- buttons so the z16->z14->z16 round-trip can be driven via `adb input tap`
            // (no multitouch pinch needed). Each tap steps zoom by 1, dwelling so tiles reload.
            val zoomOut = Button(this).apply { text = "ZOOM -"; tag = "btn_zoom_out" }
            val zoomIn = Button(this).apply { text = "ZOOM +"; tag = "btn_zoom_in" }
            val go = Button(this).apply { text = "GO"; tag = "btn_go" }
            zoomOut.setOnClickListener { map.animateCamera(CameraUpdateFactory.zoomBy(-1.0), 400) }
            zoomIn.setOnClickListener { map.animateCamera(CameraUpdateFactory.zoomBy(1.0), 400) }
            // animateCamera (unlike the init moveCamera/cameraPosition setter) reliably applies
            // post-init: jumps to a pitched z17.5 Maidan view where 3D buildings + cast shadows show.
            go.setOnClickListener {
                map.animateCamera(
                    CameraUpdateFactory.newCameraPosition(
                        CameraPosition.Builder()
                            .target(LatLng(50.4500, 30.5236))
                            .zoom(17.5)
                            .tilt(60.0)
                            .bearing(25.0)
                            .build()
                    ),
                    800
                )
            }
            val bar = LinearLayout(this).apply {
                orientation = LinearLayout.HORIZONTAL
                addView(zoomOut)
                addView(zoomIn)
                addView(go)
            }
            addContentView(
                bar,
                FrameLayout.LayoutParams(
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    FrameLayout.LayoutParams.WRAP_CONTENT,
                    Gravity.BOTTOM or Gravity.CENTER_HORIZONTAL
                )
            )
        }
    }

    override fun onStart() {
        super.onStart()
        mapView.onStart()
    }

    override fun onResume() {
        super.onResume()
        mapView.onResume()
    }

    override fun onPause() {
        super.onPause()
        mapView.onPause()
    }

    override fun onStop() {
        super.onStop()
        mapView.onStop()
    }

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    override fun onDestroy() {
        super.onDestroy()
        mapView.onDestroy()
    }

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }
}
