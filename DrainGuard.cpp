#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace drainguard {

    constexpr double DEFAULT_C = 0.5334;
    constexpr double DEFAULT_AREA_HA = 60.7028;
    constexpr double DEFAULT_DIAMETER_M = 3.590; 
    constexpr double DEFAULT_MANNING_N = 0.011;
    constexpr double DEFAULT_SLOPE = 0.01;
    constexpr double DEFAULT_ARI_YEARS = 10.0;
    constexpr double LOW_LIMIT = 0.50;
    constexpr double MEDIUM_LIMIT = 0.80;
    constexpr long double PI = 3.141592653589793238462643383279502884L;

    struct PipeCapacity {
        double area_m2 = 0.0;
        double hydraulic_radius_m = 0.0;
        double q_capacity = 0.0;
        double velocity_ms = 0.0;
    };

    struct DrainageData {
        string zone;
        double c = DEFAULT_C;
        double i_mmh = 0.0;
        double a_ha = DEFAULT_AREA_HA;
        double q_capacity = 0.0;
        double blockage_ratio = 0.0;

        double q_in = 0.0;
        double effective_q_capacity = 0.0;
        double risk_ratio = 0.0;
        string risk_level;
        string status;
        string notes;

        string rainfall_source = "Manual";
        optional<double> duration_h;
        optional<double> ari_years;
        string capacity_source = "Manual";
        optional<double> diameter_m;
        optional<double> manning_n;
        optional<double> slope;
        optional<double> pipe_area_m2;
        optional<double> hydraulic_radius_m;
        optional<double> velocity_ms;
    };

    void require_nonnegative(double value, const string& name) {
        if (!isfinite(value) || value < 0.0)
            throw invalid_argument(name + " must be a finite number >= 0.");
    }

    void require_positive(double value, const string& name) {
        if (!isfinite(value) || value <= 0.0)
            throw invalid_argument(name + " must be a finite number > 0.");
    }

    void require_fraction(double value, const string& name) {
        require_nonnegative(value, name);
        if (value > 1.0)
            throw invalid_argument(name + " must be between 0 and 1 inclusive.");
    }

    double checked_result(long double value, const string& name) {
        if (!isfinite(value) || value < 0.0L ||
            value > static_cast<long double>(numeric_limits<double>::max()))
            throw overflow_error(name + " is outside the supported numeric range.");
        const double result = static_cast<double>(value);
        if (value > 0.0L && result == 0.0)
            throw underflow_error(name + " is too small to represent reliably.");
        return result;
    }

    // Rainfall intensity in mm/h. 
    double calculate_rainfall_intensity(double duration_h,
        double ari_years = DEFAULT_ARI_YEARS) {
        require_positive(duration_h, "Storm duration (h)");
        require_positive(ari_years, "ARI (years)");
        const long double value = 59.153L * pow(static_cast<long double>(ari_years), 0.161L)
            / pow(static_cast<long double>(duration_h) + 0.118L, 0.812L);
        return checked_result(value, "Rainfall intensity");
    }

    // Discharge, not volume: mm/h * ha / 360 converts to m3/s.
    double calculate_runoff(double c, double i_mmh, double a_ha) {
        require_fraction(c, "Runoff coefficient C");
        require_nonnegative(i_mmh, "Rainfall intensity (mm/h)");
        require_positive(a_ha, "Catchment area (ha)");
        return checked_result(static_cast<long double>(c) * i_mmh * a_ha / 360.0L,
            "Runoff discharge");
    }

    PipeCapacity calculate_pipe_capacity(double diameter_m, double n, double slope) {
        require_positive(diameter_m, "Pipe diameter (m)");
        require_positive(n, "Manning roughness n");
        require_nonnegative(slope, "Slope S");
        const long double d = diameter_m;
        const long double area = PI * d * d / 4.0L;
        const long double rh = d / 4.0L;
        const long double velocity = pow(rh, 2.0L / 3.0L)
            * sqrt(static_cast<long double>(slope)) / n;
        PipeCapacity result;
        result.area_m2 = checked_result(area, "Pipe area");
        result.hydraulic_radius_m = checked_result(rh, "Hydraulic radius");
        result.velocity_ms = checked_result(velocity, "Full-flow velocity");
        result.q_capacity = checked_result(area * velocity, "Pipe capacity");
        return result;
    }

    double calculate_effective_capacity(double q_capacity, double blockage_ratio) {
        require_nonnegative(q_capacity, "Drainage capacity (m3/s)");
        require_fraction(blockage_ratio, "Blockage ratio");
        return checked_result(static_cast<long double>(q_capacity) * (1.0L - blockage_ratio),
            "Effective drainage capacity");
    }

    double calculate_risk_ratio(double q_in, double effective_q_capacity) {
        require_nonnegative(q_in, "Runoff discharge (m3/s)");
        require_nonnegative(effective_q_capacity, "Effective drainage capacity (m3/s)");
        if (effective_q_capacity == 0.0) {
            // Positive inflow / zero capacity is unbounded; 0/0 is undefined.
            return q_in > 0.0 ? numeric_limits<double>::infinity()
                : numeric_limits<double>::quiet_NaN();
        }
        return checked_result(static_cast<long double>(q_in) / effective_q_capacity,
            "Risk ratio");
    }

    string determine_risk_level(double r) {
        // NaN is used internally only for the explicitly handled 0/0 case.
        if (isnan(r)) return "Not assessed";
        if (r < 0.0) throw invalid_argument("Risk ratio cannot be negative.");
        if (r <= LOW_LIMIT) return "Low";       // 0 <= R <= 0.50
        if (r <= MEDIUM_LIMIT) return "Medium"; // 0.50 < R <= 0.80
        if (r < 1.0) return "High";             // 0.80 < R < 1.00
        return "Critical";                     // R >= 1, including positive infinity
    }

    string trim(const string& text) {
        const size_t first = text.find_first_not_of(" \t\r\n");
        if (first == string::npos) return "";
        return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
    }

    void append_note(string& notes, const string& note) {
        if (!notes.empty()) notes += " ";
        notes += note;
    }

    DrainageData evaluate_zone(DrainageData data) {
        data.zone = trim(data.zone);
        if (data.zone.empty()) throw invalid_argument("Zone name cannot be empty.");
        data.q_in = calculate_runoff(data.c, data.i_mmh, data.a_ha);
        data.effective_q_capacity = calculate_effective_capacity(data.q_capacity,
            data.blockage_ratio);
        data.risk_ratio = calculate_risk_ratio(data.q_in, data.effective_q_capacity);
        data.risk_level = determine_risk_level(data.risk_ratio);
        if (data.effective_q_capacity == 0.0) {
            data.status = data.q_in > 0.0 ? "NO_DRAINAGE_CAPACITY" : "NO_RUNOFF_NO_CAPACITY";
            append_note(data.notes, data.q_in > 0.0
                ? "Positive runoff with zero effective drainage capacity."
                : "No current runoff, but no drainage capacity; 0/0 is undefined. Inspect drainage.");
        }
        else {
            data.status = data.q_in == 0.0 ? "NO_RUNOFF" : "CALCULATED";
        }
        if (data.c == DEFAULT_C)
            append_note(data.notes, "");
        return data;
    }

    void set_idf_rainfall(DrainageData& data, double duration_h) {
        data.i_mmh = calculate_rainfall_intensity(duration_h);
        data.duration_h = duration_h;
        data.ari_years = DEFAULT_ARI_YEARS;
        data.rainfall_source = "Member A revised IDF";
    }

    void set_pipe_capacity(DrainageData& data, double d = DEFAULT_DIAMETER_M,
        double n = DEFAULT_MANNING_N, double slope = DEFAULT_SLOPE) {
        const PipeCapacity pipe = calculate_pipe_capacity(d, n, slope);
        data.capacity_source = "Manning full circular pipe";
        data.diameter_m = d;
        data.manning_n = n;
        data.slope = slope;
        data.pipe_area_m2 = pipe.area_m2;
        data.hydraulic_radius_m = pipe.hydraulic_radius_m;
        data.velocity_ms = pipe.velocity_ms;
        data.q_capacity = pipe.q_capacity;
    }

    // Numeric output is not rounded before risk classification.
    string numeric_text(double value) {
        if (isnan(value)) return "NA";
        if (isinf(value)) return "INF";
        ostringstream out;
        out.imbue(locale::classic());
        out << setprecision(numeric_limits<double>::max_digits10) << value;
        return out.str();
    }

    // Keep prompts and scenario names readable; CSV still preserves full precision.
    string short_number(double value) {
        ostringstream out;
        out.imbue(locale::classic());
        out << setprecision(10) << value;
        return out.str();
    }

    string csv_text(const string& value) {
        string result = "\"";
        for (const char c : value) {
            if (c == '"') result += '"';
            result += c;
        }
        return result + '"';
    }

    string optional_number(const optional<double>& value) {
        return value ? numeric_text(*value) : "";
    }

    void write_csv(ostream& out, const vector<DrainageData>& rows) {
        // The first ten columns retain the original program's names and order.
        out << "Zone,C,Rainfall_i_mmh,Area_ha,Q_in_m3s,Original_Q_capacity,"
            "Blockage_Ratio,Effective_Q_capacity,Risk_Ratio_R,Risk_Level,"
            "Status,Rainfall_Source,Storm_Duration_h,ARI_years,Capacity_Source,"
            "Diameter_m,Manning_n,Slope,Pipe_Area_m2,Hydraulic_Radius_m,"
            "Full_Flow_Velocity_ms,Notes\n";
        for (const auto& data : rows) {
            out << csv_text(data.zone) << ',' << numeric_text(data.c) << ','
                << numeric_text(data.i_mmh) << ',' << numeric_text(data.a_ha) << ','
                << numeric_text(data.q_in) << ',' << numeric_text(data.q_capacity) << ','
                << numeric_text(data.blockage_ratio) << ','
                << numeric_text(data.effective_q_capacity) << ','
                << numeric_text(data.risk_ratio) << ',' << csv_text(data.risk_level) << ','
                << csv_text(data.status) << ',' << csv_text(data.rainfall_source) << ','
                << optional_number(data.duration_h) << ',' << optional_number(data.ari_years) << ','
                << csv_text(data.capacity_source) << ',' << optional_number(data.diameter_m) << ','
                << optional_number(data.manning_n) << ',' << optional_number(data.slope) << ','
                << optional_number(data.pipe_area_m2) << ','
                << optional_number(data.hydraulic_radius_m) << ','
                << optional_number(data.velocity_ms) << ',' << csv_text(data.notes) << '\n';
        }
        if (!out) throw runtime_error("CSV write failed.");
    }

    void export_csv(const string& path, const vector<DrainageData>& rows) {
        ofstream out(path, ios::binary | ios::trunc);
        if (!out) throw runtime_error("Cannot open CSV output: " + path);
        write_csv(out, rows);
        out.close();
        if (!out) throw runtime_error("Could not finish writing CSV: " + path);
        cout << "\nSaved " << rows.size() << " result(s) to " << path << '\n';
    }

    void print_result(const DrainageData& data) {
        cout << fixed << setprecision(6)
            << "\nRESULTS FOR: " << data.zone
            << "\nRainfall intensity : " << data.i_mmh << " mm/h"
            << "\nRunoff discharge   : " << data.q_in << " m3/s"
            << "\nOriginal capacity  : " << data.q_capacity << " m3/s"
            << "\nEffective capacity : " << data.effective_q_capacity << " m3/s"
            << "\nCapacity source    : " << data.capacity_source
            << "\nRisk ratio R       : " << numeric_text(data.risk_ratio)
            << "\nRisk level         : " << data.risk_level
            << "\nStatus             : " << data.status << '\n';
        if (data.pipe_area_m2)
            cout << "Pipe area          : " << *data.pipe_area_m2 << " m2"
            << "\nHydraulic radius   : " << *data.hydraulic_radius_m << " m"
            << "\nFull-flow velocity : " << *data.velocity_ms << " m/s\n";
        if (!data.notes.empty()) cout << "Notes              : " << data.notes << '\n';
    }

    bool parse_number(const string& text, double& value) {
        istringstream input(trim(text));
        input.imbue(locale::classic());
        double parsed = 0.0;
        if (!(input >> parsed) || !isfinite(parsed)) return false;
        input >> ws;
        if (!input.eof()) return false; // Reject "12abc" and multiple values.
        value = parsed;
        return true;
    }

    bool read_number(const string& label, double min_value, double max_value,
        bool minimum_is_exclusive, optional<double> default_value,
        double& result) {
        string line;
        while (true) {
            cout << label;
            if (default_value) cout << " [" << short_number(*default_value) << ']';
            cout << ": " << flush;
            if (!getline(cin, line)) return false;
            if (trim(line).empty() && default_value) {
                result = *default_value;
                return true;
            }
            double value = 0.0;
            if (parse_number(line, value) && value <= max_value &&
                (minimum_is_exclusive ? value > min_value : value >= min_value)) {
                result = value;
                return true;
            }
            cout << "Invalid input. Enter one finite number "
                << (minimum_is_exclusive ? "> " : ">= ") << short_number(min_value);
            if (max_value != numeric_limits<double>::max())
                cout << " and <= " << short_number(max_value);
            cout << ". Use a decimal point, not a comma.\n";
        }
    }

    bool read_choice(const string& label, const string& allowed,
        char default_value, char& result) {
        string line;
        while (true) {
            cout << label << " [" << default_value << "]: " << flush;
            if (!getline(cin, line)) return false;
            line = trim(line);
            if (line.empty()) line = string(1, default_value);
            if (line.size() == 1 && allowed.find(line[0]) != string::npos) {
                result = line[0];
                return true;
            }
            cout << "Please choose one of: " << allowed << '\n';
        }
    }

    bool read_zone(DrainageData& data) {
        const double max_value = numeric_limits<double>::max();
        string zone;
        do {
            cout << "\nZone name (spaces allowed): " << flush;
            if (!getline(cin, zone)) return false;
            zone = trim(zone);
            if (zone.empty()) cout << "Zone name is required.\n";
        } while (zone.empty());
        data.zone = zone;
        if (!read_number("Runoff coefficient C (0 to 1)", 0, 1, false, DEFAULT_C, data.c) ||
            !read_number("Catchment area (ha)", 0, max_value, true, DEFAULT_AREA_HA, data.a_ha))
            return false;

        char mode = '1';
        if (!read_choice("Rainfall: 1=Member A IDF, 2=manual intensity", "12", '1', mode))
            return false;
        if (mode == '1') {
            double duration = 0.25;
            if (!read_number("Storm duration (hours; 0.25=15 min; ARI fixed at 10 years)",
                0.25, 72.0, false, 0.25, duration)) return false;
            set_idf_rainfall(data, duration);
        }
        else if (!read_number("Rainfall intensity (mm/h)", 0, max_value, false,
            nullopt, data.i_mmh)) return false;

        if (!read_choice("Capacity: 1=calculate from circular pipe, 2=enter supplied capacity",
            "12", '1', mode)) return false;
        if (mode == '1') {
            double diameter = DEFAULT_DIAMETER_M, n = DEFAULT_MANNING_N, slope = DEFAULT_SLOPE;
            if (!read_number("Pipe DIAMETER (m)", 0, max_value, true, DEFAULT_DIAMETER_M, diameter) ||
                !read_number("Manning roughness n", 0, max_value, true, DEFAULT_MANNING_N, n) ||
                !read_number("Slope S (m/m)", 0, max_value, false, DEFAULT_SLOPE, slope)) return false;
            set_pipe_capacity(data, diameter, n, slope);
        }
        else {
            if (!read_number("Supplied drainage capacity (m3/s)", 0, max_value, false,
                nullopt, data.q_capacity)) return false;
            append_note(data.notes, "User-supplied capacity; not verified from pipe geometry.");
        }
        return read_number("Blockage ratio (0 to 1; 0.75 means 75%)", 0, 1, false, 0,
            data.blockage_ratio);
    }

    int run_interactive(const string& output_path) {
        vector<DrainageData> rows;
        while (true) {
            try {
                DrainageData data;
                if (!read_zone(data)) {
                    cout << "\nInput ended; only completed zones will be saved.\n";
                    break;
                }
                data = evaluate_zone(data);
                print_result(data);
                rows.push_back(move(data));
            }
            catch (const exception& error) {
                cout << "\nZone was not saved: " << error.what() << '\n';
            }
            char again = 'n';
            if (!read_choice("Enter another zone? y/n", "yYnN", 'n', again) ||
                (again != 'y' && again != 'Y')) break;
        }
        if (rows.empty()) cout << "No complete valid zones; no CSV file written.\n";
        else export_csv(output_path, rows);
        return 0;
    }

    // Deterministic alternatives for Member C, not observed measurements.
    vector<DrainageData> generate_simulation_data() {
        vector<DrainageData> rows;
        for (const double duration : {0.25, 0.5, 1.0, 3.0, 6.0, 12.0, 24.0, 48.0, 72.0}) {
            DrainageData data;
            data.zone = "Campus IDF " + short_number(duration) + " h";
            set_idf_rainfall(data, duration);
            set_pipe_capacity(data);
            data.notes = "Alternative whole-campus scenario; do not sum with other campus rows.";
            rows.push_back(evaluate_zone(data));
        }
        for (const double blockage : {0.0, 0.1, 0.25, 0.5, 0.75, 0.85, 0.95, 1.0}) {
            DrainageData data;
            data.zone = "Campus peak rainfall blockage " + short_number(blockage);
            set_idf_rainfall(data, 0.25);
            set_pipe_capacity(data);
            data.blockage_ratio = blockage;
            data.notes = "Alternative whole-campus peak rainfall blockage sensitivity scenario.";
            rows.push_back(evaluate_zone(data));
        }
        for (const double blockage : {0.0, 0.25, 0.5, 1.0}) {
            DrainageData data;
            data.zone = "Campus 1 h blockage " + short_number(blockage);
            set_idf_rainfall(data, 1.0);
            set_pipe_capacity(data);
            data.blockage_ratio = blockage;
            data.notes = "Alternative whole-campus blockage sensitivity scenario.";
            rows.push_back(evaluate_zone(data));
        }
        DrainageData no_rain;
        no_rain.zone = "Campus no rain";
        set_pipe_capacity(no_rain);
        rows.push_back(evaluate_zone(no_rain));
        DrainageData no_capacity;
        no_capacity.zone = "Campus rainfall and zero capacity";
        set_idf_rainfall(no_capacity, 0.25);
        rows.push_back(evaluate_zone(no_capacity));
        DrainageData both_zero;
        both_zero.zone = "Campus no rain and zero capacity";
        rows.push_back(evaluate_zone(both_zero));

        // Three explicitly synthetic smaller zones for multi-zone integration.
        const double areas[] = { 1.2, 2.5, 0.8 };
        const double rain[] = { 20.0, 100.0, 80.0 };
        const double capacity[] = { 0.15, 0.12, 0.05 };
        const double blockage[] = { 0.0, 0.5, 0.85 };
        for (size_t index = 0; index < 3; ++index) {
            DrainageData data;
            data.zone = "Synthetic zone " + to_string(index + 1);
            data.c = 0.6;
            data.a_ha = areas[index];
            data.i_mmh = rain[index];
            data.q_capacity = capacity[index];
            data.blockage_ratio = blockage[index];
            data.notes = "Synthetic example inputs; not a surveyed campus drainage zone.";
            rows.push_back(evaluate_zone(data));
        }
        return rows;
    }

    class TestRunner {
        int passed_ = 0;
        int failed_ = 0;
    public:
        void check(const string& name, bool condition) {
            if (condition) ++passed_; else ++failed_;
            cout << (condition ? "PASS " : "FAIL ") << name << '\n';
        }
        void close(const string& name, double actual, double expected, double tolerance = 1e-9) {
            check(name, isfinite(actual) && abs(actual - expected) <= tolerance);
        }
        void rejects(const string& name, const function<void()>& action) {
            bool rejected = false;
            try { action(); }
            catch (const invalid_argument&) { rejected = true; }
            catch (const overflow_error&) { rejected = true; }
            catch (const underflow_error&) { rejected = true; }
            check(name, rejected);
        }
        int finish() const {
            cout << "\nTests passed: " << passed_ << "; failed: " << failed_ << '\n';
            return failed_ == 0 ? 0 : 1;
        }
    };

    int run_self_tests() {
        TestRunner test;
        test.close("Unit conversion: C=1, i=36 mm/h, A=1 ha gives 0.1 m3/s",
            calculate_runoff(1, 36, 1), 0.1);
        const double durations[] = { 0.25, 0.5, 1, 3, 6, 12, 24, 48, 72 };
        const double expected_i[] = { 192.98, 126.68, 78.28, 34.04, 19.69, 11.30, 6.46, 3.69, 2.66 };
        const double expected_q[] = { 17.36, 11.39, 7.04, 3.06, 1.77, 1.02, 0.58, 0.33, 0.24 };
        for (size_t index = 0; index < 9; ++index) {
            const double i = calculate_rainfall_intensity(durations[index]);
            test.close("Revised IDF " + short_number(durations[index]) + " h", i, expected_i[index], 0.0051);
            test.close("Revised runoff " + short_number(durations[index]) + " h",
                calculate_runoff(DEFAULT_C, i, DEFAULT_AREA_HA), expected_q[index], 0.0051);
        }
        const PipeCapacity pipe = calculate_pipe_capacity(DEFAULT_DIAMETER_M,
            DEFAULT_MANNING_N, DEFAULT_SLOPE);
        test.close("Circular area uses revised diameter, not radius", pipe.area_m2, 10.122290069682652);
        test.close("Full circular hydraulic radius is D/4", pipe.hydraulic_radius_m, 0.8975);
        test.close("Revised Manning capacity", pipe.q_capacity, 85.62010015745011);
        test.close("Manning velocity consistent with Q/area", pipe.velocity_ms, 8.458570103013697);
        test.close("Zero slope produces zero capacity", calculate_pipe_capacity(1, .011, 0).q_capacity, 0);
        test.close("T1 no rainfall", calculate_runoff(.5334, 0, 60.7028), 0);
        test.check("T2 low intensity with sufficient capacity",
            determine_risk_level(calculate_risk_ratio(calculate_runoff(.5, 10, 1), 1)) == "Low");
        test.check("T3 heavy rain with insufficient capacity",
            determine_risk_level(calculate_risk_ratio(calculate_runoff(1, 100, 10), .1)) == "Critical");
        test.check("T4 lower capacity raises risk ratio",
            calculate_risk_ratio(1, 2) > calculate_risk_ratio(1, 4));
        test.close("T5 blockage capacity loss", calculate_effective_capacity(20, .25), 15);
        test.check("T5 increasing blockage raises risk under linear assumption",
            calculate_risk_ratio(1, calculate_effective_capacity(2, .5)) >
            calculate_risk_ratio(1, calculate_effective_capacity(2, 0)));
        for (const auto& boundary : vector<pair<double, string>>{
                 { 0, "Low" },{ .5, "Low" },{ .500001, "Medium" },{ .8, "Medium" },
                 { .800001, "High" },{ .995, "High" },{ .9999, "High" },{ 1, "Critical" } }) {
            test.check("Risk boundary R=" + short_number(boundary.first),
                determine_risk_level(boundary.first) == boundary.second);
        }
        test.check("Immediately below 1 remains High",
            determine_risk_level(nextafter(1.0, 0.0)) == "High");
        test.check("Positive runoff / zero capacity is INF and Critical",
            isinf(calculate_risk_ratio(1, 0)) &&
            determine_risk_level(calculate_risk_ratio(1, 0)) == "Critical");
        test.check("Zero runoff / zero capacity is undefined and Not assessed",
            isnan(calculate_risk_ratio(0, 0)) &&
            determine_risk_level(calculate_risk_ratio(0, 0)) == "Not assessed");
        test.close("Zero runoff coefficient is supported", calculate_runoff(0, 20, 1), 0);
        test.rejects("Reject negative rainfall", [] { calculate_runoff(.5, -1, 1); });
        test.rejects("Reject coefficient greater than one", [] { calculate_runoff(1.2, 10, 1); });
        test.rejects("Reject negative coefficient", [] { calculate_runoff(-.1, 10, 1); });
        test.rejects("Reject zero catchment area", [] { calculate_runoff(.5, 10, 0); });
        test.rejects("Reject negative area", [] { calculate_runoff(.5, 10, -1); });
        test.rejects("Reject negative capacity", [] { calculate_effective_capacity(-1, 0); });
        test.rejects("Reject negative blockage", [] { calculate_effective_capacity(1, -.1); });
        test.rejects("Reject blockage above one", [] { calculate_effective_capacity(1, 1.1); });
        test.rejects("Reject zero diameter", [] { calculate_pipe_capacity(0, .011, .01); });
        test.rejects("Reject zero roughness", [] { calculate_pipe_capacity(1, 0, .01); });
        test.rejects("Reject negative slope", [] { calculate_pipe_capacity(1, .011, -.01); });
        test.rejects("Reject zero storm duration", [] { calculate_rainfall_intensity(0); });
        test.rejects("Reject zero ARI", [] { calculate_rainfall_intensity(1, 0); });
        test.rejects("Reject nonfinite coefficient", [] { calculate_runoff(numeric_limits<double>::quiet_NaN(), 1, 1); });
        test.rejects("Reject infinite rainfall", [] { calculate_runoff(1, numeric_limits<double>::infinity(), 1); });
        test.rejects("Reject overflowed runoff", [] { calculate_runoff(1, 1e308, 1e308); });
        test.rejects("Reject negative risk ratio", [] { determine_risk_level(-1); });
        test.rejects("Reject empty zone name", [] { DrainageData data; evaluate_zone(data); });
        double parsed = 0;
        for (const auto& text : { "abc", "12abc", "1 2", "nan", "inf", "1e309", "" })
            test.check(string("Reject numeric text '") + text + "'", !parse_number(text, parsed));
        test.check("Accept decimal/scientific notation", parse_number(" 1.25e2 ", parsed) && parsed == 125);
        test.check("CSV quotes and commas escaped", csv_text("Zone, \"A\"") == "\"Zone, \"\"A\"\"\"");
        test.check("Explicit CSV zero-capacity representations",
            numeric_text(calculate_risk_ratio(1, 0)) == "INF" &&
            numeric_text(calculate_risk_ratio(0, 0)) == "NA");
        DrainageData manual;
        manual.zone = "Manual capacity test";
        manual.i_mmh = 78.28;
        manual.q_capacity = 10.0;
        manual.blockage_ratio = .5;
        const auto manual_result = evaluate_zone(manual);
        test.close("Manual capacity scenario risk ratio", manual_result.risk_ratio, 1.40812123224);
        test.check("Manual capacity scenario is Critical", manual_result.risk_level == "Critical");
        DrainageData peak;
        peak.zone = "Revised diameter peak";
        set_idf_rainfall(peak, .25);
        set_pipe_capacity(peak);
        test.check("Revised pipe peak rainfall without blockage is Low",
            evaluate_zone(peak).risk_level == "Low");
        const auto rows = generate_simulation_data();
        test.check("Reproducible scenario count", rows.size() == 27);
        ostringstream csv;
        write_csv(csv, rows);
        test.check("CSV contains explicit capacity provenance",
            csv.str().find("Manning full circular pipe") != string::npos);
        return test.finish();
    }

    void print_help() {
        cout << "DrainGuard C++17\n"
            << "  drainguard [--interactive [output.csv]]\n"
            << "  drainguard --demo [output.csv]\n"
            << "  drainguard --self-test\n"
            << "  drainguard --help\n"
            << "Default output: drainage_simulation_results.csv in the current directory.\n"
            << "An existing output file is replaced. Use a different path to retain earlier runs.\n"
            << "--demo generates 27 deterministic, labelled simulation cases.\n";
    }

} // namespace drainguard

int main(int argc, char* argv[]) {
    try {
        const string mode = argc > 1 ? argv[1] : "--interactive";
        const string output = argc > 2 ? argv[2] : "drainage_simulation_results.csv";
        if (argc > 3 || ((mode == "--help" || mode == "--self-test") && argc > 2)) {
            drainguard::print_help();
            return 2;
        }
        if (mode == "--help") { drainguard::print_help(); return 0; }
        if (mode == "--self-test") return drainguard::run_self_tests();
        if (mode == "--interactive") return drainguard::run_interactive(output);
        if (mode == "--demo") {
            const auto rows = drainguard::generate_simulation_data();
            for (const auto& row : rows) drainguard::print_result(row);
            drainguard::export_csv(output, rows);
            return 0;
        }
        cerr << "Unknown option: " << mode << '\n';
        drainguard::print_help();
        return 2;
    }
    catch (const exception& error) {
        cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
