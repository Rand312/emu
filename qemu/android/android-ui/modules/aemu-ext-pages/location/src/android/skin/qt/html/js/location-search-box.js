class LocationSearchBox {
    init(map, searchResultsMarkers, placeChangedHandler, clearSearchResultsHandler) {
        // Add Google search box
        // Create the search box and link it to the UI element.
        var self = this;
        var searchContainer = document.getElementById('search-container');
        var input = /** @type {HTMLInputElement} */ (
            document.getElementById('search-input'));
        map.controls[google.maps.ControlPosition.TOP_CENTER].push(searchContainer);

        var autocomplete = new google.maps.places.Autocomplete(
            /** @type {HTMLInputElement} */
            (input));
        autocomplete.bindTo('bounds', map);
        // Set the data fields to return when the user selects a place.
        autocomplete.setFields(
            ['address_components', 'geometry', 'icon', 'name']);
        // Listen for the event fired when the user selects an item from the
        // pick list.
        autocomplete.addListener('place_changed', function () {
            for (marker in searchResultsMarkers) {
                marker.setMap(null);
            }
            searchResultsMarkers = [];

            var place = autocomplete.getPlace();
            if (!place.geometry) {
                // User entered the name of a Place that was not suggested and
                // pressed the Enter key, or the Place Details request failed.

                // Check if the user entered coordinates in the form of <lat>,<lng>
                function extractLatLng(coordinates) {
                    const [latStr, lngStr] = coordinates.split(',');
                    if (latStr === undefined || lngStr === undefined) {
                        console.log("Search box didn't contain coordinates");
                        return null;
                    }
                    const lat = parseFloat(latStr);
                    const lng = parseFloat(lngStr);
                    if (isNaN(lat) || isNaN(lng)) {
                        console.log("Search box didn't contain coordinates");
                        return null;
                    }
                    if (lat < -90 || lat > 90) {
                        console.log(`latitude coordinate not valid (${lat})`);
                        return null;
                    }
                    if (lng < -180 || lng > 180) {
                        console.log(`longitude coordinate not valid (${lng})`);
                        return null;
                    }
                    return { lat, lng };
                }
                var result = extractLatLng(place.name);
                if (result != null) {
                    console.log(`Search box contained coordinates (${result.lat}, ${result.lng})`);
                    placeChangedHandler(new google.maps.LatLng(result.lat, result.lng));
                }

                return;
            }

            placeChangedHandler(place.geometry.location, place.name);

            // If the place has a geometry, then present it on a map.
            if (place.geometry.viewport) {
                map.fitBounds(place.geometry.viewport);
            } else {
                map.setCenter(place.geometry.location);
                map.setZoom(17);  // Why 17? Because it looks good.
            }
        });

        document.getElementById('search-icon').addEventListener('click', function () {
            if (input.value.length > 0) {
                input.value = "";
                self._showSearchIcon();
                clearSearchResultsHandler();
            }
        });
        input.addEventListener('input', function () {
            if (input.value.length > 0) {
                self._showCloseIcon();
            } else {
                self._showSearchIcon();
            }
        });
    }

    update(address) {
        address = address || ''
        $('#search-input').val(address);
        $('#search-icon').css("display", "block");
        $('#search-spinner').css("display", "none");
        if (address.length > 0) {
            this._showCloseIcon();
        } else {
            this._showSearchIcon();
        }        
    }

    showSpinner() {
        $('#search-icon').css('display', 'none');
        $('#search-spinner').css('display', 'flex');
    }

    hideSpinner() {
        $('#search-icon').css('display', 'block');
        $('#search-spinner').css('display', 'none');
    }

    clear() {
        this._showSearchIcon();
    }

    _showSearchIcon() {
        $('#search-icon').text("search");
        $('#search-icon').css("color", "#b1b1b1");
    }

    _showCloseIcon() {
        $('#search-icon').text("close");
        $('#search-icon').css("color", "#555555");
    }
}
