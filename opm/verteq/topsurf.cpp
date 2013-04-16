// Copyright (C) 2013 Uni Research AS
// This file is licensed under the GNU General Public License v3.0

#include <opm/verteq/nav.hpp>
#include <opm/verteq/topsurf.hpp>
#include <opm/core/utility/exc.hpp>
#include <boost/range/iterator_range.hpp>
#include <algorithm> // min, max
#include <climits> // INT_MIN, INT_MAX
#include <cstdlib> // div
#include <memory> // auto_ptr

using namespace boost;
using namespace Opm;
using namespace std;

/**
 * Generic run-length iterator.
 *
 * @param ndx Index of the range to find, e.g. a column.
 *
 * @param pos Array containing accumulated counts for each range,
 * e.g. col_collpos.
 *
 * @param values Array containing the values for every range,
 * concatenated into one huge array, e.g. col_cells.
 *
 * @return Iterator that can be used to iterate through all values
 * only in the specified range, e.g. cells in a column.
 */
template <typename T> iterator_range <const T*>
run_len_iter (const int ndx, const int* const& pos, const T* const& values) {
	// skip all the values that belongs to ranges before us,
	// then we get to the start of our own range
	const T* begin_addr = &values [pos [ndx]];

	// stop when we arrive at the start of the next range
	const T* end_addr = &values [pos [ndx + 1]];

	// return an iterator over this
	return iterator_range <const T *> (begin_addr, end_addr);
}

iterator_range <const int*>
TopSurf::column (int ndx_2d) {
	return run_len_iter <const int> (ndx_2d, this->col_cellpos, this->col_cells);
}

/**
 * @brief Process to extract the top surface from a structured grid.
 *
 * This object encapsulates a procedure with variables shared amongst
 * several sub-procedures (like in Pascal). These objects are not
 * supposed to linger on afterwards.
 */
struct TopSurfBuilder {
	// source grid from which we get the input data
	const UnstructuredGrid& fine_grid;

	// target grid we are constructing
	TopSurf& ts;

	// number of grid dimensions in each direction of the plane
	Cart3D three_d;

	// dimensions needed to create a two-dimensional projection
	// of the top surface
	Cart2D two_d;

	// map from a two-dimensional Cartesian coordinate to the final
	// id of an active element in the grid, or -1 if nothing is assigned
	// this vector is first valid after elements() have been done
	vector <int> elms;

	TopSurfBuilder (const UnstructuredGrid& from, TopSurf& into)
		// link to the fine grid for the duration of the construction
		: fine_grid (from)

		// allocate memory for the grid. it is initially empty
		, ts (into)

		// extract dimensions from the source grid
		, three_d (fine_grid)
		, two_d (three_d.project ()) {

		// check that the fine grid contains structured information;
		// this is essential to mapping cells to columns
		if (!fine_grid.global_cell) {
			throw OPM_EXC ("Find grid is not (logically) structured");
		}

		// create frame of the new top surface
		dimensions ();

		// identify active columns in the grid
		elements ();
	}

	// various stages of the build process, supposed to be called in
	// this order. (I have separated them into separate procedures to
	// make it more obvious what parts that needs to be shared between
	// them)
private:
	void dimensions () {
		// we are going to create two-dimensional grid
		ts.dimensions = 2;
		ts.cartdims[0] = two_d.ni;
		ts.cartdims[1] = two_d.nj;
		ts.cartdims[2] = 1;
	}

	void elements() {
		// statistics of the deepest and highest active k-index of
		// each column in the grid. to know each index into the column,
		// we only need to know the deepest k and the count; the highest
		// is sampled to do consistency checks afterwards
		const int num_cols = two_d.num_elems ();

		// assume initially that there are no active elements in each column
		vector <int> act_cnt (num_cols, 0);
		ts.number_of_cells = 0;

		// initialize these to values that are surely out of range, so that
		// the first invocation of min or max always set the value. we use
		// this to detect whether anything was written later on. since the
		// numbering of the grid starts at the bottom, then the deepest cell
		// has the *lowest* k-index, thus we need a value greater than all
		vector <int> deep_k (num_cols, INT_MAX);
		vector <int> high_k (num_cols, INT_MIN);

		// loop once through the fine grid to gather statistics of the
		// size of the surface so we know what to allocate
		for (int fine_elem = 0; fine_elem != fine_grid.number_of_cells; ++fine_elem) {
			// get the cartesian index for this cell; this is the cell
			// number in a grid that also includes the inactive cells
			const Cart3D::elem_t cart_ndx = fine_grid.global_cell [fine_elem];

			// deconstruct the cartesian index into (i,j,k) constituents;
			// the i-index moves fastest, as this is Fortran-indexing
			const Coord3D ijk = three_d.coord (cart_ndx);

			// figure out which column this item belongs to (in 2D)
			const Cart2D::elem_t col = two_d.cart_ndx (ijk);

			// update the statistics for this column; 'deepest' is the lowest
			// k-index seen so far, 'highest' is the highest (ehm)
			deep_k[col] = min (deep_k[col], ijk.k);
			high_k[col] = max (high_k[col], ijk.k);

			// we have seen an element in this column; it becomes active. only
			// columns with active cells will get active elements in the surface
			// grid. if this cell wasn't marked as active before we can check it
			// off now
			if (!act_cnt[col]++) {
				ts.number_of_cells++;
			}
		}

		// check that we have a continuous range of elements in each column;
		// this must be the case to assume that the entire column can be merged
		for (int col = 0; col < num_cols; ++col) {
			if (act_cnt[col]) {
				if (deep_k[col] + act_cnt[col] - 1 != high_k[col]) {
					const Coord2D coord = two_d.coord (col);
					throw OPM_EXC ("Non-continuous column at (%d, %d)", coord.i, coord.j);
				}
			}
		}

		// allocate memory needed to hold the elements in the grid structure
		// if we throw an exception at some point, the destructor of the TopSurf
		// will take care of deallocating this memory for us
		ts.global_cell = new int [ts.number_of_cells];
		ts.col_cellpos = new int [ts.number_of_cells+1];

		// there is no elements before the first column, so this number is
		// always zero. it is written to the array to maintain a straight code
		// path in calculations.
		ts.col_cellpos[0] = 0;

		// now we know enough to start assigning ids to active columns.if
		// memory is a real shortage, we could reuse the act_cnt array for this.
		elms.resize (num_cols, -1);

		// loop through the grid and assign an id for all columns that have
		// active elements
		int elem_id = 0;
		for (int col = 0; col < num_cols; ++col) {
			if (act_cnt[col]) {
				elms[col] = elem_id;

				// dual pointer that maps the other way; what is the structured
				// index of this element (flattened into an integer)
				ts.global_cell[elem_id] = col;

				// note the number of elements there now are before the next column;
				// in addition to all the previous ones, our elements are now added
				ts.col_cellpos[elem_id+1] = ts.col_cellpos[elem_id] + act_cnt[col];
				elem_id++;
			}
		}

		// now write indices from the fine grid into the column map of the surface
		// we end up with a list of element that are in each column
		ts.col_cells = new int [fine_grid.number_of_cells];
		for (int cell = 0; cell < fine_grid.number_of_cells; ++cell) {
			// get the Cartesian index for this element
			const Cart3D::elem_t cart_ndx = fine_grid.global_cell[cell];
			const Coord3D ijk = three_d.coord (cart_ndx);

			// get the id of the column in which this element now belongs
			const Cart2D::elem_t col = two_d.cart_ndx (ijk);
			const int elem_id = elms[col];

			// start of the list of elements for this particular column
			const int segment = ts.col_cellpos[elem_id];

			// since there is supposed to be a continuous range of elements in
			// each column, we can calculate the relative position in the list
			// based on the k part of the coordinate.
			const int offset = ijk.k - deep_k[col];

			// write the fine grid cell number in the column list; since we
			// have calculated the position based on depth, the list will be
			// sorted downwards up when we are done
			ts.col_cells[segment + offset] = cell;
		}

		// these members will be filled by computeGeometry, but needs valid
		// memory to work with
		ts.cell_volumes = new double [ts.number_of_cells];
		ts.cell_centroids = new double [ts.dimensions * ts.number_of_cells];
	}
};

TopSurf*
TopSurf::create (const UnstructuredGrid& fine_grid) {
	// I *know* that we are not supposed to use auto_ptr anymore, but
	// it works in both C++98 and C++11 and is the only common way to
	// transfer the object out of the smart pointer afterwards
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
	auto_ptr <TopSurf> ts (new TopSurf);
#pragma GCC diagnostic pop

	// outsource the entire construction to a builder object
	TopSurfBuilder (fine_grid, *(ts.get ()));

	// client owns pointer to constructed grid from this point
	return ts.release ();
}

TopSurf::TopSurf ()
	: col_cells (0)
	, col_cellpos (0) {
	// zero initialize all members that come from UnstructuredGrid
	// since that struct is a C struct, it doesn't have a ctor
	dimensions = 0;
	number_of_cells = 0;
	number_of_faces = 0;
	number_of_nodes = 0;
	face_nodes = 0;
	face_nodepos = 0;
	face_cells = 0;
	cell_faces = 0;
	cell_facepos = 0;
	node_coordinates = 0;
	face_centroids = 0;
	face_areas = 0;
	face_normals = 0;
	cell_centroids = 0;
	cell_volumes = 0;
	global_cell = 0;
	cartdims[0] = 0;
	cartdims[1] = 0;
	cartdims[2] = 0;
	cell_facetag = 0;
}

TopSurf::~TopSurf () {
	// deallocate memory that may have been created. if the dtor is
	// called from throwing an exception, the members should be zero
	// initialized, so it's OK to send them to delete.
	delete [] face_nodes;
	delete [] face_nodepos;
	delete [] face_cells;
	delete [] cell_faces;
	delete [] cell_facepos;
	delete [] node_coordinates;
	delete [] face_centroids;
	delete [] face_areas;
	delete [] face_normals;
	delete [] cell_volumes;
	delete [] global_cell;
	delete [] cell_facetag;
	// these are the extra members that are TopSurf specific
	delete [] col_cells;
	delete [] col_cellpos;
}
