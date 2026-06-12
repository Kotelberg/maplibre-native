package org.maplibre.android.testapp.activity.customlayer

import android.os.Bundle
import android.view.Menu
import android.view.MenuItem
import android.view.View
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.android.material.floatingactionbutton.FloatingActionButton
import org.maplibre.android.camera.CameraPosition
import org.maplibre.android.camera.CameraUpdateFactory
import org.maplibre.android.geometry.LatLng
import org.maplibre.android.maps.MapView
import org.maplibre.android.maps.MapLibreMap
import org.maplibre.android.maps.Style
import org.maplibre.android.style.layers.DebugCubeLayer
import org.maplibre.android.testapp.R
import org.maplibre.android.testapp.model.customlayer.ExampleCustomLayer

/**
 * Test activity showcasing the Custom Layer API
 *
 *
 * Note: experimental API, do not use.
 *
 */
class CustomLayerActivity : AppCompatActivity() {
    private lateinit var maplibreMap: MapLibreMap
    private lateinit var mapView: MapView
    private var cubeLayer: DebugCubeLayer? = null
    private lateinit var fab: FloatingActionButton
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_custom_layer)
        mapView = findViewById(R.id.mapView)
        mapView.onCreate(savedInstanceState)
        mapView.getMapAsync { map: MapLibreMap ->
            maplibreMap = map
            // M1 debug cube QA: HataHub style (fill-extrusion buildings) at Kyiv center
            maplibreMap.moveCamera(
                CameraUpdateFactory.newCameraPosition(
                    CameraPosition.Builder()
                        .target(LatLng(50.4501, 30.5234))
                        .zoom(16.0)
                        .tilt(55.0)
                        .build()
                )
            )
            maplibreMap.setStyle(
                Style.Builder().fromUri("https://map.hatahub.com.ua/style/light.json")
            ) { _: Style? ->
                initFab()
                // M1 QA: add the debug cube immediately so headless adb QA
                // does not depend on tapping the FAB.
                swapCustomLayer()
            }
        }
    }

    private fun initFab() {
        fab = findViewById(R.id.fab)
        fab.setColorFilter(ContextCompat.getColor(this, R.color.primary))
        fab.setOnClickListener { _: View? ->
            if (this::maplibreMap.isInitialized) {
                swapCustomLayer()
            }
        }
    }

    private fun swapCustomLayer() {
        val style = maplibreMap.style
        if (cubeLayer != null) {
            style!!.removeLayer(cubeLayer!!)
            cubeLayer = null
            fab.setImageResource(R.drawable.ic_layers)
        } else {
            cubeLayer = DebugCubeLayer("debug-cube", 50.4501, 30.5234, 50.0)
            style!!.addLayer(cubeLayer!!)
            fab.setImageResource(R.drawable.ic_layers_clear)
        }
    }

    private fun updateLayer() {
        if (this::maplibreMap.isInitialized) {
            maplibreMap.triggerRepaint()
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

    override fun onSaveInstanceState(outState: Bundle) {
        super.onSaveInstanceState(outState)
        mapView.onSaveInstanceState(outState)
    }

    override fun onDestroy() {
        super.onDestroy()
        mapView.onDestroy()
    }

    override fun onLowMemory() {
        super.onLowMemory()
        mapView.onLowMemory()
    }

    override fun onCreateOptionsMenu(menu: Menu): Boolean {
        menuInflater.inflate(R.menu.menu_custom_layer, menu)
        return true
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        return when (item.itemId) {
            R.id.action_update_layer -> {
                updateLayer()
                true
            }
            R.id.action_set_color_red -> {
                ExampleCustomLayer.setColor(1f, 0f, 0f, 1f)
                true
            }
            R.id.action_set_color_green -> {
                ExampleCustomLayer.setColor(0f, 1f, 0f, 1f)
                true
            }
            R.id.action_set_color_blue -> {
                ExampleCustomLayer.setColor(0f, 0f, 1f, 1f)
                true
            }
            else -> super.onOptionsItemSelected(item)
        }
    }
}
