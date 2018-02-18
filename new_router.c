#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "extract.h"
#include "segment.h"
#include "placer.h"
#include "router.h"
#include "heap.h"
#include "blif.h"
#include "usage_matrix.h"
#include "maze_router.h"
#include "dumb_router.h"
#include "util.h"

static struct coordinate check_offsets[] = {
	{0, 0, 0}, // here
	{-1, 0, 0}, // below
	{0, 1, 0}, // north
	{-1, 1, 0}, // below-north
	{0, 0, 1}, // east
	{-1, 0, 1}, // below-east
	{0, -1, 0}, // south
	{-1, -1, 0}, // below-south
	{0, 0, -1}, // west
	{-1, 0, -1} // below-west
};


static int interrupt_routing = 0;

static void router_sigint_handler(int a)
{
	printf("Interrupt\n");
	interrupt_routing = 1;
}

void print_routed_segment(struct routed_segment *rseg)
{
	assert(rseg->n_coords >= 0);
	for (int i = 0; i < rseg->n_coords; i++)
		printf("(%d, %d, %d) ", rseg->coords[i].y, rseg->coords[i].z, rseg->coords[i].x);
	printf("\n");
}

void print_routed_net(struct routed_net *rn)
{
	int j = 0;
	for (struct routed_segment_head *rsh = rn->routed_segments; rsh; rsh = rsh->next, j++) {
		printf("[maze_route] net %d, segment %d: ", rn->net, j);
		print_routed_segment(&rsh->rseg);
	}
}

void print_routings(struct routings *rt)
{
	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		print_routed_net(&rt->routed_nets[i]);
	}
}

void free_routings(struct routings *rt)
{
	free(rt->routed_nets);
	free(rt);
}

/* the pointer arithmetic to copy parent/child references works because
   they point exclusively into the routings structure and its sub-structures. */
struct routings *copy_routings(struct routings *old_rt)
{
	struct routings *new_rt = malloc(sizeof(struct routings));
	new_rt->n_routed_nets = old_rt->n_routed_nets;
	new_rt->routed_nets = malloc((new_rt->n_routed_nets + 1) * sizeof(struct routed_net));
	new_rt->npm = old_rt->npm;

	/* for each routed_net in routings */
	for (net_t i = 1; i < new_rt->n_routed_nets + 1; i++) {
		struct routed_net *rn = &(new_rt->routed_nets[i]);
		struct routed_net on = old_rt->routed_nets[i];
		rn->n_pins = on.n_pins;
		rn->pins = malloc(rn->n_pins * sizeof(struct placed_pin));
		memcpy(rn->pins, on.pins, sizeof(struct placed_pin) * rn->n_pins);
		rn->routed_segments = NULL;
/*
		for (int j = 0; j < rn->n_pins; j++)
			rn->pins[j].parent = on.pins[j].parent - on.routed_segments + rn->routed_segments;
*/

		abort(); // TODO: implement copying segments

/*
		// for each routed_segment in routed_net
		for (int j = 0; j < rn->n_routed_segments; j++) {
			struct routed_segment *rseg = &(rn->routed_segments[j]);
			struct routed_segment old_rseg = on.routed_segments[j];
			rseg->seg = old_rseg.seg;
			rseg->n_coords = old_rseg.n_coords;
			rseg->coords = malloc(sizeof(struct coordinate) * rseg->n_coords);
			memcpy(rseg->coords, old_rseg.coords, sizeof(struct coordinate) * rseg->n_coords);
			rseg->score = old_rseg.score;
			rseg->net = rn;
			rseg->n_child_segments = old_rseg.n_child_segments;
			rseg->n_child_pins = old_rseg.n_child_pins;

			// copy parent, child segments, and child pins
			rseg->parent = old_rseg.parent - on.routed_segments + rn->routed_segments;
			for (int k = 0; k < rseg->n_child_segments; k++)
				rseg->child_segments[k] = old_rseg.child_segments[k] - on.routed_segments + rn->routed_segments;
			for (int k = 0; k < rseg->n_child_pins; k++)
				rseg->child_pins[k] = old_rseg.child_pins[k] - on.pins + rn->pins;
			
		}
*/
	}

	return new_rt;
}

void routings_displace(struct routings *rt, struct coordinate disp)
{
	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		struct routed_net *rn = &(rt->routed_nets[i]);
		for (struct routed_segment_head *rsh = rn->routed_segments; rsh; rsh = rsh->next) {
			struct routed_segment *rseg = &rsh->rseg;
			for (int k = 0; k < rseg->n_coords; k++) {
				rseg->coords[k] = coordinate_add(rseg->coords[k], disp);
			}
		}

		for (int j = 0; j < rn->n_pins; j++)
			rn->pins[j].coordinate = coordinate_add(rn->pins[j].coordinate, disp);

		/* displace pins separately, as segments refer to them possibly more than once */
		for (int j = 0; j < rt->npm->n_pins_for_net[i]; j++) {
			struct placed_pin *p = &(rt->npm->pins[i][j]);
			p->coordinate = coordinate_add(p->coordinate, disp);
		}
	}
}

struct dimensions compute_routings_dimensions(struct routings *rt)
{
	struct coordinate d = {0, 0, 0};
	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		struct routed_net rn = rt->routed_nets[i];
		for (struct routed_segment_head *rsh = rn.routed_segments; rsh; rsh = rsh->next) {
			struct routed_segment rseg = rsh->rseg;
			for (int k = 0; k < rseg.n_coords; k++) {
				struct coordinate c = rseg.coords[k];
				d = coordinate_piecewise_max(d, c);
			}

			d = coordinate_piecewise_max(d, rseg.seg.start);
			d = coordinate_piecewise_max(d, rseg.seg.end);
		}
	}

	/* the dimension is the highest coordinate, plus 1 on each */
	struct dimensions dd = {d.y + 1, d.z + 1, d.x + 1};

	return dd;
}

/* maze routing routines */
typedef unsigned int visitor_t;
visitor_t to_visitor_t(unsigned int i) {
	return i + 1;
}

unsigned int from_visitor_t(visitor_t i) {
	return i - 1;
}

int segment_routed(struct routed_segment *rseg)
{
	struct segment seg = rseg->seg;
	struct coordinate s = seg.start, e = seg.end;
	return (s.y | s.z | s.x | e.y | e.z | e.x);
}

void routed_net_add_segment_node(struct routed_net *rn, struct routed_segment_head *rsh)
{
	assert(rsh->next == NULL);

	struct routed_segment_head *tail = rn->routed_segments;
	if (!tail) {
		rn->routed_segments = rsh;
		return;
	}

	while (tail->next)
		tail = tail->next;

	tail->next = rsh;
}


/* net scoring routines */

static int max_net_score = -1;
static int min_net_score = -1;
static int total_nets = 0;

static int count_routings_violations(struct cell_placements *cp, struct routings *rt, FILE *log)
{
	total_nets = 0;
	max_net_score = min_net_score = -1;

	/* ensure no coordinate is < 0 */
	struct coordinate tlcp = placements_top_left_most_point(cp);
	struct coordinate tlrt = routings_top_left_most_point(rt);
	struct coordinate top_left_most = coordinate_piecewise_min(tlcp, tlrt);
/*
	printf("[count_routings_violations] tlcp x: %d, y: %d, z: %d\n", tlcp.x, tlcp.y, tlcp.z);
	printf("[count_routings_violations] tlrt x: %d, y: %d, z: %d\n", tlrt.x, tlrt.y, tlrt.z);
	printf("[count_routings_violations] top_left_most x: %d, y: %d, z: %d\n", top_left_most.x, top_left_most.y, top_left_most.z);
*/
	assert(top_left_most.x >= 0 && top_left_most.y >= 0 && top_left_most.z >= 0);

	struct dimensions d = dimensions_piecewise_max(compute_placement_dimensions(cp), compute_routings_dimensions(rt));

	int usage_size = d.y * d.z * d.x;
	unsigned char *matrix = malloc(usage_size * sizeof(unsigned char));
	memset(matrix, 0, usage_size * sizeof(unsigned char));

	/* placements */
	for (int i = 0; i < cp->n_placements; i++) {
		struct placement p = cp->placements[i];
		struct coordinate c = p.placement;
		struct dimensions pd = p.cell->dimensions[p.turns];

		int cell_x = c.x + pd.x;
		int cell_y = c.y + pd.y;
		int cell_z = c.z + pd.z;

		int z1 = max(0, c.z), z2 = min(d.z, cell_z);
		int x1 = max(0, c.x), x2 = min(d.x, cell_x);

		for (int y = c.y; y < cell_y; y++) {
			for (int z = z1; z < z2; z++) {
				for (int x = x1; x < x2; x++) {
					int idx = y * d.z * d.x + z * d.x + x;

					matrix[idx]++;
				}
			}
		}
	}

	int total_violations = 0;

	/* segments */
	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		struct routed_net *rnet = &(rt->routed_nets[i]);
		int score = 0;

		for (struct routed_segment_head *rsh = rnet->routed_segments; rsh; rsh = rsh->next, total_nets++) {
			int segment_violations = 0;

			struct routed_segment *rseg = &rsh->rseg;

			for (int k = 0; k < rseg->n_coords; k++) {
				struct coordinate c = rseg->coords[k];
				// printf("[crv] c = (%d, %d, %d)\n", c.y, c.z, c.x);

				int block_in_violation = 0;
				for (int m = 0; m < sizeof(check_offsets) / sizeof(struct coordinate); m++) {
					struct coordinate cc = coordinate_add(c, check_offsets[m]);
					// printf("[crv] cc = (%d, %d, %d)\n", cc.y, cc.z, cc.x);

					// ignore if checking out of bounds
					if (cc.y < 0 || cc.y >= d.y || cc.z < 0 || cc.z >= d.z || cc.x < 0 || cc.x >= d.x) {
						// printf("[crv] oob\n");
						continue;
					}

					// only ignore the start/end pins for first/last blocks on net
					if (k == 0 || k == rseg->n_coords - 1) {
						int skip = 0;
						for (int n = 0; n < rnet->n_pins; n++) {
							struct coordinate pin_cc = rnet->pins[n].coordinate;
							if (coordinate_equal(cc, pin_cc))
								skip++;
							pin_cc.y--;
							if (coordinate_equal(cc, pin_cc))
								skip++;
						}

						if (skip)
							continue;
					}

					int idx = (cc.y * d.z * d.x) + (cc.z * d.x) + cc.x;

					// do not mark or it will collide with itself
					if (matrix[idx]) {
						block_in_violation++;
						// printf("[crv] violation\n");
						fprintf(log, "[violation] by net %d, seg %p at (%d, %d, %d) with (%d, %d, %d)\n",
						              i, (void *)rseg, c.y, c.z, c.x, cc.y, cc.z, cc.x);
					}
				}

				if (block_in_violation) {
					segment_violations++;
					total_violations++;
				}
			}

			// printf("[crv] segment_violations = %d\n", segment_violations);

			int segment_score = segment_violations * 1000 + rseg->n_coords;
			rseg->score = segment_score;
			score += segment_score;
			fprintf(log, "[crv] net %d seg %p score = %d\n", i, (void *)rseg, segment_score);
		}

		/* second loop actually marks segment in matrix */
		for (struct routed_segment_head *rsh = rnet->routed_segments; rsh; rsh = rsh->next) {
			struct routed_segment *rseg = &rsh->rseg;

			for (int k = 0; k < rseg->n_coords; k++) {
				struct coordinate c = rseg->coords[k];
				int idx = (c.y * d.z * d.x) + (c.z * d.x) + c.x;
				matrix[idx]++;

				if (c.y - 1 > 0) {
					idx = (c.y - 1) * d.z * d.x + c.z * d.x + c.x;
					matrix[idx]++;
				}
			}

		}

		if (max_net_score == -1) {
			max_net_score = min_net_score = score;
		} else {
			max_net_score = max(score, max_net_score);
			min_net_score = min(score, min_net_score);
		}
	}

	free(matrix);

	return total_violations;
}

void mark_routing_congestion(struct coordinate c, struct dimensions d, unsigned int *congestion, unsigned char *visited)
{
	int margin = 1;
	int z_start = max(c.z - margin, 0);
	int z_end = min(c.z + margin, d.z - 1);
	int x_start = max(c.x - margin, 0);
	int x_end = min(c.x + margin, d.x - 1);

	for (int z = z_start; z <= z_end; z++)
		for (int x = x_start; x <= x_end; x++)
			if (!visited[z * d.x + x]++)
				congestion[z * d.x + x]++;
}

/* create a table showing the congestion of a routing by showing where
   nets are currently being routed */
void print_routing_congestion(struct routings *rt)
{
	struct dimensions d = compute_routings_dimensions(rt);
	unsigned int *congestion = calloc(d.x * d.z, sizeof(unsigned int));

	// avoid marking a net over itself
	unsigned char *visited = calloc(d.x * d.z, sizeof(unsigned char));

	for (net_t i = 1; i < rt->n_routed_nets; i++)
		for (struct routed_segment_head *rsh = rt->routed_nets[i].routed_segments; rsh; rsh = rsh->next) {
			for (int k = 0; k < rsh->rseg.n_coords; k++)
				mark_routing_congestion(rsh->rseg.coords[k], d, congestion, visited);

			memset(visited, 0, sizeof(unsigned char) * d.x * d.z);
		}

	free(visited);

	printf("[routing_congestion] Routing congestion appears below:\n");
	printf("[routing_congestion] Z X ");
	for (int x = 0; x < d.x; x++)
		printf("%3d ", x);
	printf("\n");

	for (int z = 0; z < d.z; z++) {
		printf("[routing_congestion] %3d ", z);
		for (int x = 0; x < d.x; x++)
			printf("%3d ", congestion[z * d.x + x]);
		printf("\n");
	}
	printf("\n");

	free(congestion);
}

/* rip-up and natural selection routines */
struct rip_up_set {
	int n_ripped;
	struct routed_segment **rip_up;
};

struct routed_segment_head *remove_rsh(struct routed_segment *rseg)
{
	struct routed_net *rn = rseg->net;
	// find the previous element to delete this element
	struct routed_segment_head *node = NULL;

	if (!rn->routed_segments)
		return NULL;

	if (&(rn->routed_segments->rseg) == rseg) {
		node = rn->routed_segments;
		rn->routed_segments = node->next;
	} else {
		struct routed_segment_head *prev;
		for (prev = rn->routed_segments; prev; prev = prev->next) {
			if (&(prev->next->rseg) == rseg)
				break;
		}
		assert(prev);

		node = prev->next;
		prev->next = node->next;
	}

	node->next = NULL;
	return node;
}

// it's important to maintain the order of the routed segments in the routed
// net because the rip_up set struct relies on pointers
void rip_up_segment(struct routed_segment *rseg)
{
	// unlink segments' parent pointer here
	for (int i = 0; i < rseg->n_child_segments; i++) {
		struct routed_segment *child_rseg = rseg->child_segments[i];
		child_rseg->parent = NULL;
	}
	if (rseg->child_segments)
		free(rseg->child_segments);
	rseg->child_segments = NULL;
	rseg->n_child_segments = 0;

	// unlink pins' parent pointer here
	for (int i = 0; i < rseg->n_child_pins; i++) {
		struct placed_pin *child_pin = rseg->child_pins[i];
		child_pin->parent = NULL;
	}
	if (rseg->child_pins)
		free(rseg->child_pins);
	rseg->child_pins = NULL;
	rseg->n_child_pins = 0;

	// find and remove this segment from its parent
	struct routed_segment *parent_rseg = rseg->parent;
	if (parent_rseg) {
		for (int i = 0; i < parent_rseg->n_child_segments; i++) {
			if (parent_rseg->child_segments[i] == rseg) {
				if (i < parent_rseg->n_child_segments - 1)
					parent_rseg->child_segments[i] = parent_rseg->child_segments[parent_rseg->n_child_segments - 1];
				else
					parent_rseg->child_segments[i] = NULL;

				parent_rseg->n_child_segments--;
				break;
			}
			// this search should succeed
			assert(i < parent_rseg->n_child_segments - 1);
		}
	}

	assert(rseg->coords);
	free(rseg->coords);
	rseg->coords = NULL;
	rseg->n_coords = 0;
	struct segment zero = {{0, 0, 0}, {0, 0, 0}};
	rseg->seg = zero;
	rseg->score = 0;
}

// to sort in descending order, reverse the subtraction
int rseg_score_cmp(const void *a, const void *b)
{
	struct routed_segment *aa = *(struct routed_segment **)a;
	struct routed_segment *bb = *(struct routed_segment **)b;

	return bb->score - aa->score;
}

static struct rip_up_set natural_selection(struct routings *rt, FILE *log)
{
	int rip_up_count = 0;
	int rip_up_size = 4;
	struct routed_segment **rip_up = calloc(rip_up_size, sizeof(struct routed_segment *));

	int score_range = max_net_score - min_net_score;
	int bias = score_range / 8;
	int random_range = bias * 10;

	fprintf(log, "[natural_selection] adjusted_score = score - %d (min net score) + %d (bias)\n", min_net_score, bias);
	fprintf(log, "[natural_selection] net   seg                rip   rand(%5d)   adj. score\n", score_range);
	fprintf(log, "[natural_selection] ---   ----------------   ---   -----------   ----------\n");

	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		for (struct routed_segment_head *rsh = rt->routed_nets[i].routed_segments; rsh; rsh = rsh->next) {
			struct routed_segment *rseg = &rsh->rseg;
			if (!segment_routed(rseg))
				continue;

			int r = random() % random_range;
			int adjusted_score = rseg->score - min_net_score + bias;

			if (r < adjusted_score) {
				if (log)
					// fprintf(log, "[natural_selection] ripping up net %d (rand(%d) = %d < %d)\n", i, random_range, r, adjusted_score);
					fprintf(log, "[natural_selection] %3d   %p    X         %5d   %5d\n", i, (void *)rseg, r, adjusted_score);
#ifdef NATURAL_SELECTION_DEBUG
				printf("[natural_selection] ripping up net %2d, segment %p (rand(%d) = %d < %d)\n", i, rseg, random_range, r, adjusted_score);
#endif
				// print_routed_segment(&rt->routed_nets[i].routed_segments[j]);
				rip_up[rip_up_count++] = rseg;
				if (rip_up_count >= rip_up_size) {
					rip_up_size *= 2;
					rip_up = realloc(rip_up, rip_up_size * sizeof(struct routed_segment *));
				}
			} else {
#ifdef NATURAL_SELECTION_DEBUG
				printf("[natural_selection] leaving intact net %2d, segment %p (rand(%d) = %d >= %d)\n", i, rseg, random_range, r, adjusted_score);
#endif
				if (log)
					fprintf(log, "[natural_selection] %3d   %p               %5d   %5d\n", i, (void *)rseg, r, adjusted_score);
					// fprintf(log, "[natural_selection] leaving net %d intact (rand(%d) = %d >= %d)\n", i, random_range, r, adjusted_score);
			}
		}
	}

	struct rip_up_set rus = {rip_up_count, rip_up};

	return rus;
}

static struct routings *initial_route(struct blif *blif, struct net_pin_map *npm)
{
	struct routings *rt = malloc(sizeof(struct routings));
	rt->n_routed_nets = npm->n_nets;
	rt->routed_nets = calloc(rt->n_routed_nets + 1, sizeof(struct routed_net));
	rt->npm = npm;

	for (net_t i = 1; i < npm->n_nets + 1; i++)
		dumb_route(&rt->routed_nets[i], blif, npm, i);

	return rt;
}

void assert_in_bounds(struct routed_net *rn)
{
	int arbitrary_max = 1000;
	for (struct routed_segment_head *rsh = rn->routed_segments; rsh; rsh = rsh->next) {
		struct routed_segment *rseg = &rsh->rseg;
		if (segment_routed(rseg)) {
			for (int j = 0; j < rseg->n_coords; j++) {
				struct coordinate c = rseg->coords[j];
				assert(c.y >= 0 && c.z >= 0 && c.x >= 0 && c.y < arbitrary_max && c.z < arbitrary_max && c.x < arbitrary_max);
			}
		}
	}
}

/* main route subroutine */
struct routings *route(struct blif *blif, struct cell_placements *cp)
{
	struct pin_placements *pp = placer_place_pins(cp);
	struct net_pin_map *npm = placer_create_net_pin_map(pp);

	struct routings *rt = initial_route(blif, npm);
	print_routings(rt);
	recenter(cp, rt, 2);

	int iterations = 0;
	int violations = 0;

	interrupt_routing = 0;
	signal(SIGINT, router_sigint_handler);
	FILE *log = fopen("router.log", "w");

	violations = count_routings_violations(cp, rt, log);

	printf("\n");
	while ((violations = count_routings_violations(cp, rt, log)) > 0 && !interrupt_routing) {
		struct rip_up_set rus = natural_selection(rt, log);
		// sort elements by highest score
		qsort(rus.rip_up, rus.n_ripped, sizeof(struct routed_segment *), rseg_score_cmp);

		struct routed_net **nets_ripped = calloc(rus.n_ripped, sizeof(struct routed_net *));

		printf("\r[router] Iterations: %4d, Violations: %d, Segments to re-route: %d", iterations + 1, violations, rus.n_ripped);
		fprintf(log, "\n[router] Iterations: %4d, Violations: %d, Segments to re-route: %d\n", iterations + 1, violations, rus.n_ripped);
		fflush(stdout);
		fflush(log);

		// rip up all segments in rip-up set
		for (int i = 0; i < rus.n_ripped; i++) {
			fprintf(log, "[router] Ripping up net %d, segment %p (score %d)\n", rus.rip_up[i]->net->net, (void *)rus.rip_up[i], rus.rip_up[i]->score);
			nets_ripped[i] = rus.rip_up[i]->net;
			rip_up_segment(rus.rip_up[i]);

			// remove segment from rt
			struct routed_segment_head *rsh = remove_rsh(rus.rip_up[i]);
			free(rsh);
		}

		// individually reroute all net instances that have had rip-ups occur
		for (int i = 0; i < rus.n_ripped; i++) {
			struct routed_net *net_to_reroute = nets_ripped[i];
			if (!net_to_reroute)
				continue;

			fprintf(log, "[router] Rerouting net %d\n", net_to_reroute->net);

			recenter(cp, rt, 2);
			maze_reroute(cp, rt, net_to_reroute, 2);

			// prevent subsequent reroutings of this net
			for (int j = i + 1; j < rus.n_ripped; j++)
				if (nets_ripped[j] == net_to_reroute)
					nets_ripped[j] = NULL;

			// printf("[maze_reroute] Rerouted net %d\n", net_to_reroute->net);
			// print_routed_net(net_to_reroute);
			assert_in_bounds(net_to_reroute);
		}
		free(rus.rip_up);
		rus.n_ripped = 0;

		recenter(cp, rt, 2);

		iterations++;
	}

	signal(SIGINT, SIG_DFL);

	printf("\n[router] Solution found! Optimizing...\n");
	fprintf(log, "\n[router] Solution found! Optimizing...\n");
	fclose(log);

/*
	// rip up a net wholesale and reroute it
	for (net_t i = 1; i < rt->n_routed_nets + 1; i++) {
		struct routed_segment_head *next = rt->routed_nets[i].routed_segments, *curr;
		do {
			curr = next;
			next = next->next;
			rip_up_segment(&curr->rseg);
			free(curr);
		} while (next);
		rt->routed_nets[i].routed_segments = NULL;

		recenter(cp, rt, 2);
		maze_reroute(cp, rt, &rt->routed_nets[i], 2);

		violations = count_routings_violations(cp, rt, log);
		printf("[optimizing] net %d: %d violations (should be none)\n", i, violations);
		assert(violations == 0);
	}
*/

	printf("[router] Routing complete!\n");
	print_routings(rt);

	free_pin_placements(pp);
	// free_net_pin_map(npm); // screws with extract in vis_png

	return rt;
}
