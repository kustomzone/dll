//=======================================================================
// Copyright (c) 2014 Baptiste Wicht
// Distributed under the terms of the MIT License.
// (See accompanying file LICENSE or copy at
//  http://opensource.org/licenses/MIT)
//=======================================================================

#ifndef DLL_DBN_INL
#define DLL_DBN_INL

#include <tuple>

#include "cpp_utils/tuple_utils.hpp"

#include "unit_type.hpp"
#include "dbn_trainer.hpp"
#include "conjugate_gradient.hpp"
#include "dbn_common.hpp"
#include "svm_common.hpp"

namespace dll {

template<typename Layer, cpp_enable_if(layer_traits<Layer>::is_pooling_layer())>
void store_layer(const Layer&, std::ostream&){}

template<typename Layer, cpp_disable_if(layer_traits<Layer>::is_pooling_layer())>
void store_layer(const Layer& layer, std::ostream& os){
    layer.store(os);
}

template<typename Layer, cpp_enable_if(layer_traits<Layer>::is_pooling_layer())>
void load_layer(Layer&, std::istream&) {}

template<typename Layer, cpp_disable_if(layer_traits<Layer>::is_pooling_layer())>
void load_layer(Layer& layer, std::istream& is){
    layer.load(is);
}

template<typename Layer, typename Iterator, typename Enable = void>
struct input_converter {
    using container = decltype(std::declval<Layer>().convert_input(std::declval<Iterator>(), std::declval<Iterator>()));

    container c;

    input_converter(Layer& layer, Iterator first, Iterator last){
        c = layer.convert_input(first, last);
    }

    auto begin(){
        return c.begin();
    }

    auto end(){
        return c.end();
    }
};

template<typename Layer, typename Iterator>
struct input_converter <Layer, Iterator, std::enable_if_t<std::is_same<typename Layer::input_one_t, typename Iterator::value_type>::value>> {
    Iterator first;
    Iterator last;

    input_converter(Layer& /*layer*/, Iterator first, Iterator last) : first(first), last(last) {
        //Nothing else to init
    }

    Iterator begin(){
        return first;
    }

    Iterator end(){
        return last;
    }
};

/*!
 * \brief A Deep Belief Network implementation
 */
template<typename Desc>
struct dbn final {
    using desc = Desc;
    using this_type = dbn<desc>;

    using tuple_type = typename desc::layers::tuple_type;
    tuple_type tuples;

    static constexpr const std::size_t layers = desc::layers::layers;

    template <std::size_t N>
    using rbm_type = typename std::tuple_element<N, tuple_type>::type;

    //TODO Could be good to ensure that either a) all rbm have the same weight b) use the correct type for each rbm
    using weight = typename rbm_type<0>::weight;

    using watcher_t = typename desc::template watcher_t<this_type>;

    weight learning_rate = 0.77;

    weight initial_momentum = 0.5;      ///< The initial momentum
    weight final_momentum = 0.9;        ///< The final momentum applied after *final_momentum_epoch* epoch
    weight final_momentum_epoch = 6;    ///< The epoch at which momentum change

    weight weight_cost = 0.0002;        ///< The weight cost for weight decay

    weight momentum = 0;                ///< The current momentum

    thread_pool<dbn_traits<this_type>::is_parallel()> pool;

#ifdef DLL_SVM_SUPPORT
    svm::model svm_model;               ///< The learned model
    svm::problem problem;               ///< libsvm is stupid, therefore, you cannot destroy the problem if you want to use the model...
    bool svm_loaded = false;            ///< Indicates if a SVM model has been loaded (and therefore must be saved)
#endif //DLL_SVM_SUPPORT

    //No arguments by default
    template<cpp_disable_if_cst(dbn_traits<this_type>::is_dynamic())>
    dbn(){}

//Note: The tuple implementation of Clang and G++ seems highly
//different. Indeed, g++ only allows to forward arguments to the
//constructors if they are directly convertible.

#ifdef __clang__
    template<typename... T, cpp_enable_if_cst(dbn_traits<this_type>::is_dynamic())>
    explicit dbn(T&&... rbms) : tuples(std::forward<T>(rbms)...) {
        //Nothing else to init
    }
#else
    template<typename... T, cpp_enable_if_cst(dbn_traits<this_type>::is_dynamic())>
    explicit dbn(T&&... rbms) : tuples({std::forward<T>(rbms)}...) {
        //Nothing else to init
    }
#endif

    //No copying
    dbn(const dbn& dbn) = delete;
    dbn& operator=(const dbn& dbn) = delete;

    //No moving
    dbn(dbn&& dbn) = delete;
    dbn& operator=(dbn&& dbn) = delete;

    void display() const {
        std::size_t parameters = 0;

        std::cout << "DBN with " << layers << " layers" << std::endl;

        cpp::for_each(tuples, [&parameters](auto& rbm){
            std::cout << "\t";
            parameters += rbm.parameters();
            rbm.display();
        });

        std::cout << "Total parameters: " << parameters << std::endl;
    }

    void store(const std::string& file) const {
        std::ofstream os(file, std::ofstream::binary);
        store(os);
    }

    void load(const std::string& file){
        std::ifstream is(file, std::ifstream::binary);
        load(is);
    }

    void store(std::ostream& os) const {
        cpp::for_each(tuples, [&os](auto& layer){
            store_layer(layer, os);
        });

#ifdef DLL_SVM_SUPPORT
        svm_store(*this, os);
#endif //DLL_SVM_SUPPORT
    }

    void load(std::istream& is){
        cpp::for_each(tuples, [&is](auto& layer){
            load_layer(layer, is);
        });

#ifdef DLL_SVM_SUPPORT
        svm_load(*this, is);
#endif //DLL_SVM_SUPPORT
    }

    template<std::size_t N>
    auto layer() -> typename std::add_lvalue_reference<rbm_type<N>>::type {
        return std::get<N>(tuples);
    }

    template<std::size_t N>
    constexpr auto layer() const -> typename std::add_lvalue_reference<typename std::add_const<rbm_type<N>>::type>::type {
        return std::get<N>(tuples);
    }

    template<std::size_t N>
    static constexpr std::size_t layer_input_size() noexcept {
        return layer_traits<rbm_type<N>>::input_size();
    }

    template<std::size_t N>
    static constexpr std::size_t layer_output_size() noexcept {
        return layer_traits<rbm_type<N>>::output_size();
    }

    static constexpr std::size_t input_size() noexcept {
        return layer_traits<rbm_type<0>>::input_size();
    }

    static constexpr std::size_t output_size() noexcept {
        return layer_traits<rbm_type<layers - 1>>::output_size();
    }

    static std::size_t full_output_size() noexcept {
        std::size_t output = 0;
        for_each_type<tuple_type>([&output](auto* rbm){
            output += std::decay_t<std::remove_pointer_t<decltype(rbm)>>::output_size();
        });
        return output;
    }

    /*{{{ Pretrain */

    template<std::size_t I, class Enable = void>
    struct train_next;

    template<std::size_t I>
    struct train_next<I, std::enable_if_t<(I < layers - 1)>> : std::true_type {};

    template<std::size_t I>
    struct train_next<I, std::enable_if_t<(I == layers - 1)>> : cpp::bool_constant<layer_traits<rbm_type<I>>::pretrain_last()> {};

    template<std::size_t I>
    struct train_next<I, std::enable_if_t<(I > layers - 1)>> : std::false_type {};


    template<typename Iterator>
    std::size_t fast_distance(Iterator& first, Iterator& last){
        if(std::is_same<typename std::iterator_traits<Iterator>::iterator_category, std::random_access_iterator_tag>::value){
            return std::distance(first, last);
        } else {
            return 0;
        }
    }

    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<(I<layers)> pretrain_layer(Iterator first, Iterator last, Watcher& watcher, std::size_t max_epochs){
        using rbm_t = rbm_type<I>;
        using input_t = typename rbm_t::input_t;

        decltype(auto) rbm = layer<I>();

        watcher.template pretrain_layer<rbm_t>(*this, I, fast_distance(first, last));

        rbm.template train<
            !watcher_t::ignore_sub, //Enable the RBM Watcher or not
            dbn_detail::rbm_watcher_t<watcher_t>> //Replace the RBM watcher if not void
                (first, last, max_epochs);

        if(train_next<I+1>::value){
            auto next_a = rbm.prepare_output(std::distance(first, last));

            maybe_parallel_foreach_i(pool, first, last, [&rbm, &next_a](auto& v, std::size_t i){
                rbm.activate_one(v, next_a[i]);
            });

            pretrain_layer<I+1>(next_a.begin(), next_a.end(), watcher, max_epochs);
        }
    }

    //Stop template recursion
    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<(I==layers)> pretrain_layer(Iterator, Iterator, Watcher&, std::size_t){}

    template<std::size_t I, class Enable = void>
    struct batch_layer_ignore : std::false_type {};

    template<std::size_t I>
    struct batch_layer_ignore<I, std::enable_if_t<(I < layers)>> : cpp::bool_constant_c<cpp::or_u<layer_traits<rbm_type<I>>::is_pooling_layer(), !layer_traits<rbm_type<I>>::pretrain_last()>> {};

    //Special handling for the layer 0
    //data is coming from iterators not from input
    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<(I==0)> pretrain_layer_batch(Iterator first, Iterator last, Watcher& watcher, std::size_t max_epochs){
        using rbm_t = rbm_type<I>;

        decltype(auto) rbm = layer<I>();

        watcher.template pretrain_layer<rbm_t>(*this, I, 0);

        using rbm_trainer_t = dll::rbm_trainer<rbm_t, !watcher_t::ignore_sub, dbn_detail::rbm_watcher_t<watcher_t>>;

        //Initialize the RBM trainer
        rbm_trainer_t r_trainer;

        //Init the RBM and training parameters
        r_trainer.init_training(rbm, first, last); //TODO This may be highly slow...

        //Get the specific trainer (CD)
        auto trainer = rbm_trainer_t::template get_trainer<false>(rbm);

        auto big_batch_size = desc::BatchSize * get_batch_size(rbm);

        //Train for max_epochs epoch
        for(std::size_t epoch = 0; epoch < max_epochs; ++epoch){
            std::size_t big_batch = 0;

            //Create a new context for this epoch
            rbm_training_context context;

            r_trainer.init_epoch();

            auto it = first;
            auto end = last;

            while(it != end){
                auto batch_start = it;

                std::size_t i = 0;
                while(it != end && i < big_batch_size){
                    ++it;
                    ++i;
                }

                decltype(auto) input = rbm.convert_input(batch_start, it);

                //Train the RBM on this big batch
                r_trainer.train_sub(input.begin(), input.end(), input.begin(), trainer, context, rbm);

                //TODO Move that to the watcher
                std::cout << "DBN: Pretraining batch " << big_batch << std::endl;

                ++big_batch;
            }

            r_trainer.finalize_epoch(epoch, context, rbm);
        }

        r_trainer.finalize_training(rbm);

        pretrain_layer_batch<I+1>(first, last, watcher, max_epochs);
    }

    //Special handling for pooling layers
    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<batch_layer_ignore<I>::value> pretrain_layer_batch(Iterator first, Iterator last, Watcher& watcher, std::size_t max_epochs){
        //We simply go up one layer on pooling layers
        pretrain_layer_batch<I+1>(first, last, watcher, max_epochs);
    }

    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<(I>0 && I<layers && !batch_layer_ignore<I>::value)> pretrain_layer_batch(Iterator first, Iterator last, Watcher& watcher, std::size_t max_epochs){
        using rbm_t = rbm_type<I>;

        decltype(auto) rbm = layer<I>();

        watcher.template pretrain_layer<rbm_t>(*this, I, 0);

        using rbm_trainer_t = dll::rbm_trainer<rbm_t, !watcher_t::ignore_sub, dbn_detail::rbm_watcher_t<watcher_t>>;

        //Initialize the RBM trainer
        rbm_trainer_t r_trainer;

        //Init the RBM and training parameters
        r_trainer.init_training(rbm, first, last);

        //Get the specific trainer (CD)
        auto trainer = rbm_trainer_t::template get_trainer<false>(rbm);

        auto big_batch_size = desc::BatchSize * get_batch_size(rbm);

        auto activated_input = layer<I - 1>().prepare_output(big_batch_size);

        //Train for max_epochs epoch
        for(std::size_t epoch = 0; epoch < max_epochs; ++epoch){
            std::size_t big_batch = 0;

            //Create a new context for this epoch
            rbm_training_context context;

            r_trainer.init_epoch();

            auto it = first;
            auto end = last;

            while(it != end){
                auto batch_start = it;

                std::size_t i = 0;
                while(it != end && i < big_batch_size){
                    ++it;
                    ++i;
                }

                auto input = layer<0>().convert_input(batch_start, it);

                //Collect a big batch
                maybe_parallel_foreach_i(pool, input.begin(), input.end(), [this,&activated_input](auto& v, std::size_t i){
                    this->activation_probabilities<0, I>(v, activated_input[i]);
                });

                //Train the RBM on this big batch
                r_trainer.train_sub(activated_input.begin(), activated_input.end(), activated_input.begin(), trainer, context, rbm);

                //TODO Move that to the watcher
                std::cout << "DBN: Pretraining batch " << big_batch << std::endl;

                ++big_batch;
            }

            r_trainer.finalize_epoch(epoch, context, rbm);
        }

        r_trainer.finalize_training(rbm);

        //train the next layer, if any
        pretrain_layer_batch<I+1>(first, last, watcher, max_epochs);
    }

    //Stop template recursion
    template<std::size_t I, typename Iterator, typename Watcher>
    std::enable_if_t<(I==layers && !batch_layer_ignore<I>::value)> pretrain_layer_batch(Iterator, Iterator, Watcher&, std::size_t){}

    /*!
     * \brief Pretrain the network by training all layers in an unsupervised
     * manner.
     */
    template<typename Samples>
    void pretrain(const Samples& training_data, std::size_t max_epochs){
        pretrain(training_data.begin(), training_data.end(), max_epochs);
    }

    /*!
     * \brief Pretrain the network by training all layers in an unsupervised
     * manner.
     */
    template<typename Iterator>
    void pretrain(Iterator&& first, Iterator&& last, std::size_t max_epochs){
        watcher_t watcher;

        watcher.pretraining_begin(*this, max_epochs);

        //Pretrain each layer one-by-one
        if(dbn_traits<this_type>::save_memory()){
            std::cout << "DBN: Pretraining done in batch mode to save memory" << std::endl;
            pretrain_layer_batch<0>(std::forward<Iterator>(first), std::forward<Iterator>(last), watcher, max_epochs);
        } else {
            //Convert data to an useful form
            input_converter<rbm_type<0>, Iterator> converter(layer<0>(), std::forward<Iterator>(first), std::forward<Iterator>(last));

            pretrain_layer<0>(converter.begin(), converter.end(), watcher, max_epochs);
        }

        watcher.pretraining_end(*this);
    }

    /*}}}*/

    /*{{{ Train with labels */

    //Note: dyn_vector cannot be replaced with fast_vector, because labels is runtime

    template<typename Samples, typename Labels>
    void train_with_labels(const Samples& training_data, const Labels& training_labels, std::size_t labels, std::size_t max_epochs){
        cpp_assert(training_data.size() == training_labels.size(), "There must be the same number of values than labels");
        cpp_assert(dll::input_size(layer<layers - 1>()) == dll::output_size(layer<layers - 2>()) + labels, "There is no room for the labels units");

        train_with_labels(training_data.begin(), training_data.end(), training_labels.begin(), training_labels.end(), labels, max_epochs);
    }

    template<std::size_t I, typename Input, typename Watcher, typename LabelIterator>
    std::enable_if_t<(I<layers)> train_with_labels(const Input& input, Watcher& watcher, LabelIterator lit, LabelIterator lend, std::size_t labels, std::size_t max_epochs){
        using rbm_t = rbm_type<I>;

        decltype(auto) rbm = layer<I>();

        watcher.template pretrain_layer<rbm_t>(*this, I, input.size());

        rbm.template train<
            !watcher_t::ignore_sub, //Enable the RBM Watcher or not
            dbn_detail::rbm_watcher_t<watcher_t>> //Replace the RBM watcher if not void
                (input, max_epochs);

        if(I < layers - 1){
            bool is_last = I == layers - 2;

            auto next_a = rbm.prepare_output(input.size());
            auto next_s = rbm.prepare_output(input.size());

            rbm.activate_many(input, next_a, next_s);

            if(is_last){
                auto big_next_a = rbm.prepare_output(input.size(), is_last, labels);

                //Cannot use std copy since the sub elements have different size
                for(std::size_t i = 0; i < next_a.size(); ++i){
                    for(std::size_t j = 0; j < next_a[i].size(); ++j){
                        big_next_a[i][j] = next_a[i][j];
                    }
                }

                std::size_t i = 0;
                while(lit != lend){
                    decltype(auto) label = *lit;

                    for(size_t l = 0; l < labels; ++l){
                        big_next_a[i][dll::output_size(rbm) + l] = label == l ? 1.0 : 0.0;
                    }

                    ++i;
                    ++lit;
                }

                train_with_labels<I+1>(big_next_a, watcher, lit, lend, labels, max_epochs);
            } else {
                train_with_labels<I+1>(next_a, watcher, lit, lend, labels, max_epochs);
            }
        }
    }

    template<std::size_t I, typename Input, typename Watcher, typename LabelIterator>
    std::enable_if_t<(I==layers)> train_with_labels(Input&, Watcher&, LabelIterator, LabelIterator, std::size_t, std::size_t){}

    template<typename Iterator, typename LabelIterator>
    void train_with_labels(Iterator&& first, Iterator&& last, LabelIterator&& lfirst, LabelIterator&& llast, std::size_t labels, std::size_t max_epochs){
        cpp_assert(std::distance(first, last) == std::distance(lfirst, llast), "There must be the same number of values than labels");
        cpp_assert(dll::input_size(layer<layers - 1>()) == dll::output_size(layer<layers - 2>()) + labels, "There is no room for the labels units");

        watcher_t watcher;

        watcher.pretraining_begin(*this, max_epochs);

        //Convert data to an useful form
        auto data = layer<0>().convert_input(std::forward<Iterator>(first), std::forward<Iterator>(last));

        train_with_labels<0>(data, watcher, std::forward<LabelIterator>(lfirst), std::forward<LabelIterator>(llast), labels, max_epochs);

        watcher.pretraining_end(*this);
    }

    /*}}}*/

    /*{{{ Predict with labels */

    template<std::size_t I, typename Input, typename Output>
    std::enable_if_t<(I<layers)> predict_labels(const Input& input, Output& output, std::size_t labels) const {
        decltype(auto) rbm = layer<I>();

        auto next_a = rbm.prepare_one_output();
        auto next_s = rbm.prepare_one_output();

        rbm.activate_hidden(next_a, next_s, input, input);

        if(I == layers - 1){
            auto output_a = rbm.prepare_one_input();
            auto output_s = rbm.prepare_one_input();

            rbm.activate_visible(next_a, next_s, output_a, output_s);

            output = std::move(output_a);
        } else {
            bool is_last = I == layers - 2;

            //If the next layers is the last layer
            if(is_last){
                auto big_next_a = rbm.prepare_one_output(is_last, labels);

                for(std::size_t i = 0; i < next_a.size(); ++i){
                    big_next_a[i] = next_a[i];
                }

                //std::copy(next_a.begin(), next_a.end(), big_next_a.begin() + dll::output_size(rbm));
                std::fill(big_next_a.begin() + dll::output_size(rbm), big_next_a.end(), 0.1);

                predict_labels<I+1>(big_next_a, output, labels);
            } else {
                predict_labels<I+1>(next_a, output, labels);
            }
        }
    }

    template<std::size_t I, typename Input, typename Output>
    std::enable_if_t<(I==layers)> predict_labels(const Input&, Output&, std::size_t) const {}

    template<typename TrainingItem>
    size_t predict_labels(const TrainingItem& item_data, std::size_t labels) const {
        cpp_assert(dll::input_size(layer<layers - 1>()) == dll::output_size(layer<layers - 2>()) + labels, "There is no room for the labels units");

        typename rbm_type<0>::input_one_t item(item_data);

        auto output_a = layer<layers - 1>().prepare_one_input();

        predict_labels<0>(item, output_a, labels);

        return std::distance(
            std::prev(output_a.end(), labels),
            std::max_element(std::prev(output_a.end(), labels), output_a.end()));
    }

    /*}}}*/

    /*{{{ Predict */

    template<std::size_t I, std::size_t S = layers, typename Input, typename Result>
    std::enable_if_t<(I<S)> activation_probabilities(const Input& input, Result& result) const {
        auto& rbm = layer<I>();

        if(I < S - 1){
            auto next_a = rbm.prepare_one_output();
            rbm.activate_one(input, next_a);
            activation_probabilities<I+1, S>(next_a, result);
        } else {
            rbm.activate_one(input, result);
        }
    }

    //Stop template recursion
    template<std::size_t I, std::size_t S = layers, typename Input, typename Result>
    std::enable_if_t<(I==S)> activation_probabilities(const Input&, Result&) const {}

    template<typename Sample, typename Output>
    void activation_probabilities(const Sample& item_data, Output& result) const {
        auto data = layer<0>().convert_sample(item_data);

        activation_probabilities<0>(data, result);
    }

    template<typename Sample>
    auto activation_probabilities(const Sample& item_data) const {
        auto result = layer<layers - 1>().prepare_one_output();

        activation_probabilities(item_data, result);

        return result;
    }

    template<std::size_t I, typename Input, typename Result>
    std::enable_if_t<(I<layers)> full_activation_probabilities(const Input& input, std::size_t& i, Result& result) const {
        auto& rbm = layer<I>();

        auto next_s = rbm.prepare_one_output();
        auto next_a = rbm.prepare_one_output();

        rbm.activate_one(input, next_a, next_s);

        for(auto& value : next_a){
            result[i++] = value;
        }

        full_activation_probabilities<I+1>(next_a, i, result);
    }

    //Stop template recursion
    template<std::size_t I, typename Input, typename Result>
    std::enable_if_t<(I==layers)> full_activation_probabilities(const Input&, std::size_t&, Result&) const {}

    template<typename Sample, typename Output>
    void full_activation_probabilities(const Sample& item_data, Output& result) const {
        auto data = layer<0>().convert_sample(item_data);

        std::size_t i = 0;

        full_activation_probabilities<0>(data, i, result);
    }

    template<typename Sample>
    etl::dyn_vector<weight> full_activation_probabilities(const Sample& item_data) const {
        etl::dyn_vector<weight> result(full_output_size());

        full_activation_probabilities(item_data, result);

        return result;
    }

    template<typename Sample, typename DBN = this_type, cpp::enable_if_u<dbn_traits<DBN>::concatenate()> = cpp::detail::dummy>
    auto get_final_activation_probabilities(const Sample& sample) const {
        return full_activation_probabilities(sample);
    }

    template<typename Sample, typename DBN = this_type, cpp::disable_if_u<dbn_traits<DBN>::concatenate()> = cpp::detail::dummy>
    auto get_final_activation_probabilities(const Sample& sample) const {
        return activation_probabilities(sample);
    }

    template<typename Weights>
    size_t predict_label(const Weights& result) const {
        return std::distance(result.begin(), std::max_element(result.begin(), result.end()));
    }

    template<typename Sample>
    size_t predict(const Sample& item) const {
        auto result = activation_probabilities(item);
        return predict_label(result);;
    }

    /*}}}*/

    /*{{{ Fine-tuning */

    template<typename Samples, typename Labels>
    weight fine_tune(const Samples& training_data, Labels& labels, size_t max_epochs, size_t batch_size){
        return fine_tune(training_data.begin(), training_data.end(), labels.begin(), labels.end(), max_epochs, batch_size);
    }

    template<typename Iterator, typename LIterator>
    weight fine_tune(Iterator&& first, Iterator&& last, LIterator&& lfirst, LIterator&& llast, size_t max_epochs, size_t batch_size){
        dll::dbn_trainer<this_type> trainer;
        return trainer.train(*this,
            std::forward<Iterator>(first), std::forward<Iterator>(last),
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            max_epochs, batch_size);
    }

    /*}}}*/

    using output_one_t = typename rbm_type<layers - 1>::output_one_t;
    using output_t = typename rbm_type<layers - 1>::output_one_t;

    output_one_t prepare_one_output() const {
        return layer<layers - 1>().prepare_one_output();
    }

#ifdef DLL_SVM_SUPPORT

    /*{{{ SVM Training and prediction */

    using svm_samples_t = std::conditional_t<
        dbn_traits<this_type>::concatenate(),
        std::vector<etl::dyn_vector<weight>>,     //In full mode, use a simple 1D vector
        typename rbm_type<layers - 1>::output_t>; //In normal mode, use the output of the last layer

    template<typename DBN = this_type, typename Result, typename Sample, cpp::enable_if_u<dbn_traits<DBN>::concatenate()> = cpp::detail::dummy>
    void add_activation_probabilities(Result& result, const Sample& sample){
        result.emplace_back(full_output_size());
        full_activation_probabilities(sample, result.back());
    }

    template<typename DBN = this_type, typename Result, typename Sample, cpp::disable_if_u<dbn_traits<DBN>::concatenate()> = cpp::detail::dummy>
    void add_activation_probabilities(Result& result, const Sample& sample){
        result.push_back(layer<layers - 1>().prepare_one_output());
        activation_probabilities(sample, result.back());
    }

    template<typename Samples, typename Labels>
    void make_problem(const Samples& training_data, const Labels& labels, bool scale = false){
        svm_samples_t svm_samples;

        //Get all the activation probabilities
        for(auto& sample : training_data){
            add_activation_probabilities(svm_samples, sample);
        }

        //static_cast ensure using the correct overload
        problem = svm::make_problem(labels, static_cast<const svm_samples_t&>(svm_samples), scale);
    }

    template<typename Iterator, typename LIterator>
    void make_problem(Iterator first, Iterator last, LIterator&& lfirst, LIterator&& llast, bool scale = false){
        svm_samples_t svm_samples;

        //Get all the activation probabilities
        std::for_each(first, last, [this, &svm_samples](auto& sample){
            this->add_activation_probabilities(svm_samples, sample);
        });

        //static_cast ensure using the correct overload
        problem = svm::make_problem(
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            svm_samples.begin(), svm_samples.end(),
            scale);
    }

    template<typename Samples, typename Labels>
    bool svm_train(const Samples& training_data, const Labels& labels, const svm_parameter& parameters = default_svm_parameters()){
        cpp::stop_watch<std::chrono::seconds> watch;

        make_problem(training_data, labels, dbn_traits<this_type>::scale());

        //Make libsvm quiet
        svm::make_quiet();

        //Make sure parameters are not messed up
        if(!svm::check(problem, parameters)){
            return false;
        }

        //Train the SVM
        svm_model = svm::train(problem, parameters);

        svm_loaded = true;

        std::cout << "SVM training took " << watch.elapsed() << "s" << std::endl;

        return true;
    }

    template<typename Iterator, typename LIterator>
    bool svm_train(Iterator&& first, Iterator&& last, LIterator&& lfirst, LIterator&& llast, const svm_parameter& parameters = default_svm_parameters()){
        cpp::stop_watch<std::chrono::seconds> watch;

        make_problem(
            std::forward<Iterator>(first), std::forward<Iterator>(last),
            std::forward<LIterator>(lfirst), std::forward<LIterator>(llast),
            dbn_traits<this_type>::scale());

        //Make libsvm quiet
        svm::make_quiet();

        //Make sure parameters are not messed up
        if(!svm::check(problem, parameters)){
            return false;
        }

        //Train the SVM
        svm_model = svm::train(problem, parameters);

        svm_loaded = true;

        std::cout << "SVM training took " << watch.elapsed() << "s" << std::endl;

        return true;
    }

    template<typename Samples, typename Labels>
    bool svm_grid_search(const Samples& training_data, const Labels& labels, std::size_t n_fold = 5, const svm::rbf_grid& g = svm::rbf_grid()){
        make_problem(training_data, labels, dbn_traits<this_type>::scale());

        //Make libsvm quiet
        svm::make_quiet();

        auto parameters = default_svm_parameters();

        //Make sure parameters are not messed up
        if(!svm::check(problem, parameters)){
            return false;
        }

        //Perform a grid-search
        svm::rbf_grid_search(problem, parameters, n_fold, g);

        return true;
    }

    template<typename It, typename LIt>
    bool svm_grid_search(It&& first, It&& last, LIt&& lfirst, LIt&& llast, std::size_t n_fold = 5, const svm::rbf_grid& g = svm::rbf_grid()){
        make_problem(
            std::forward<It>(first), std::forward<It>(last),
            std::forward<LIt>(lfirst), std::forward<LIt>(llast),
            dbn_traits<this_type>::scale());

        //Make libsvm quiet
        svm::make_quiet();

        auto parameters = default_svm_parameters();

        //Make sure parameters are not messed up
        if(!svm::check(problem, parameters)){
            return false;
        }

        //Perform a grid-search
        svm::rbf_grid_search(problem, parameters, n_fold, g);

        return true;
    }

    template<typename Sample>
    double svm_predict(const Sample& sample){
        auto features = get_final_activation_probabilities(sample);
        return svm::predict(svm_model, features);
    }

    /*}}}*/

#endif //DLL_SVM_SUPPORT

};

} //end of namespace dll

#endif
